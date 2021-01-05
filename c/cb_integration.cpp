#include "cb_integration.h"
#include "cb_bst.h"
#include "cb_term.h"

#include <assert.h>
#include <stdio.h>
#include "compiler.h"
#include "object.h"
#include "memory.h"
#include "value.h"
#include "vm.h"
#include "trace.h"

#include <atomic>
#include <chrono>
#include <thread>

__thread struct cb        *thread_cb            = NULL;
__thread void             *thread_ring_start;
__thread cb_mask_t         thread_ring_mask;
__thread struct cb_region  thread_region;
__thread cb_offset_t       thread_cutoff_offset = (cb_offset_t)0ULL;
__thread struct ObjTable   thread_objtable;

//NOTE: For tandem allocations not yet having a presence in the VM state, we
// need to temporarily hold any new_lower_bound until the tandem allocations
// are completed, such that the latter allocations amongst the tandem set of
// allocations don't accidentally clobber the earlier ones if a GC were to be
// provoked.  'pin_new_lower_bound' is used for this reason.
__thread cb_offset_t       pinned_lower_bound   = CB_NULL;

__thread bool              on_main_thread = false;
__thread bool              can_print      = false;
__thread cb_offset_t       thread_objtable_lower_bound;

static __thread struct rcbp      *thread_rcbp_list        = NULL;


struct cb        *gc_thread_cb            = NULL;
struct cb_region  gc_thread_region;
cb_offset_t       gc_thread_darkset_bst   = CB_BST_SENTINEL;

static std::thread gc_thread;
static std::atomic<bool> gc_stop_flag(false);
static std::atomic<struct gc_request_response*> gc_current_request(0);
static std::atomic<struct gc_request_response*> gc_current_response(0);
struct gc_request_response* gc_last_processed_response = NULL;
bool gc_request_is_outstanding;


int exec_phase = EXEC_PHASE_COMPILE;
int gc_phase = GC_PHASE_NORMAL_EXEC;
bool is_resizing = false;


scoped_pin::scoped_pin(const char *func, int line) {
  func_ = func;
  line_ = line;
  prev_pin_offset_ = pinned_lower_bound;
  curr_pin_offset_ = cb_region_cursor(&thread_region);

  KLOX_TRACE("begin pin @ %ju (%s:%d)\n", (uintmax_t)curr_pin_offset_, func_, line_);
  assert(pinned_lower_bound == CB_NULL || cb_offset_cmp(prev_pin_offset_, curr_pin_offset_) == -1 || cb_offset_cmp(prev_pin_offset_, curr_pin_offset_) == 0);
  if (pinned_lower_bound == CB_NULL)
    pinned_lower_bound = curr_pin_offset_;
}

scoped_pin::~scoped_pin() {
  KLOX_TRACE("end pin @ %ju (%s:%d)\n", (uintmax_t)curr_pin_offset_, func_, line_);
  assert(cb_offset_cmp(pinned_lower_bound, curr_pin_offset_) == -1 || cb_offset_cmp(pinned_lower_bound, curr_pin_offset_) == 0);
  pinned_lower_bound = prev_pin_offset_;
}


void
rcbp_add(struct rcbp *item) {
  if (thread_rcbp_list)
    thread_rcbp_list->prev_ = item;
  item->next_ = thread_rcbp_list;
  item->prev_ = NULL;
  thread_rcbp_list = item;
}

void
rcbp_remove(struct rcbp *item) {
  if (item->prev_)
    item->prev_->next_ = item->next_;
  if (item->next_)
    item->next_->prev_ = item->prev_;
  if (thread_rcbp_list == item)
    thread_rcbp_list = item->next_;
}

void
rcbp_rewrite_list(struct cb *new_cb)
{
  struct rcbp *item = thread_rcbp_list;

  KLOX_TRACE("BEGIN REWRITE LIST\n");
  while (item) {
    if (item->offset_ != CB_NULL) {
      void *new_pointer = cb_at(new_cb, item->offset_);

      KLOX_TRACE("Rewriting pointer %p of cb:%p to %p of new_cb:%p\n",
                 item->pointer_, item->cb_, new_pointer, new_cb);

      item->pointer_ = new_pointer;
      item->cb_ = new_cb;
    } else {
      KLOX_TRACE("rewrite list item %p has CB_NULL offset, so not rewriting.\n", item);
    }

    item = item->next_;
  }
  KLOX_TRACE("END REWRITE LIST\n");
}


size_t
klox_no_external_size(const struct cb      *cb,
                      const struct cb_term *term)
{
  return 0;
}

size_t
klox_no_external_size2(const struct cb *cb,
                       uint64_t         offset)
{
  return 0;
}

static size_t
klox_Obj_external_size(const struct cb *cb,
                       Obj *obj)
{
  //NOTE: This must be a quick O(1) calculation to maintain performance; it is
  // inappropriate for this to recursively calculate size information.
  //NOTE: The objtable's present external size will be maintained on insertions
  // and removals of entries to it, as well as in-place mutations to its
  // contained entries.  There will be entries (in particular OBJ_CLASS,
  // OBJ_FUNCTION, and OBJ_INSTANCE) which will have such in-place
  // mutations.  In such cases, the objtable's tracking of its own size cannot
  // be performed solely upon insertions/removals of its contained objects;
  // additional tracking (via cb_bst_external_size_adjust()) must be done at
  // these points of mutations.

  switch (obj->type) {
    case OBJ_BOUND_METHOD:
      return sizeof(ObjBoundMethod) + cb_alignof(ObjBoundMethod) - 1
        + alloc_header_size + alloc_header_align - 1;

    case OBJ_CLASS: {
      ObjClass *clazz = (ObjClass *)obj;
      return sizeof(ObjClass) + cb_alignof(ObjClass) - 1
        + structmap_internal_size(&(clazz->methods_sm))
        + alloc_header_size + alloc_header_align - 1;
    }

    case OBJ_CLOSURE: {
      ObjClosure *closure = (ObjClosure *)obj;
      return sizeof(ObjClosure) + cb_alignof(ObjClosure) - 1
        + (closure->upvalueCount * sizeof(OID<ObjUpvalue>)) + cb_alignof(OID<ObjUpvalue>) - 1
        + (2 * (alloc_header_size + alloc_header_align - 1));
    }

    case OBJ_FUNCTION: {
      ObjFunction *function = (ObjFunction *)obj;
      return sizeof(ObjFunction) + cb_alignof(ObjFunction) - 1
             + function->chunk.capacity * sizeof(uint8_t) + cb_alignof(uint8_t) - 1         //code
             + function->chunk.capacity * sizeof(int) + cb_alignof(int) - 1                 //lines
             + function->chunk.constants.capacity * sizeof(Value) + cb_alignof(Value) - 1  //constants.values
             + (4 * (alloc_header_size + alloc_header_align - 1));
    }

    case OBJ_INSTANCE: {
      ObjInstance *instance = (ObjInstance *)obj;
      return sizeof(ObjInstance) + cb_alignof(ObjInstance) - 1
             + structmap_internal_size(&(instance->fields_sm))
             + alloc_header_size + alloc_header_align - 1;
    }

    case OBJ_NATIVE:
      return sizeof(ObjNative) + cb_alignof(ObjNative) - 1
        + alloc_header_size + alloc_header_align - 1;

    case OBJ_STRING: {
      ObjString *str = (ObjString *)obj;
      return (sizeof(ObjString) + cb_alignof(ObjString) - 1
        + (str->length + 1) * sizeof(char))
        + (2 * (alloc_header_size + alloc_header_align - 1));
    }

    case OBJ_UPVALUE:
      return sizeof(ObjUpvalue) + cb_alignof(ObjUpvalue) - 1
        + alloc_header_size + alloc_header_align - 1;

    default:
      KLOX_TRACE("Unrecognized type: '%c'\n", (char)obj->type);
      assert(obj->type == OBJ_BOUND_METHOD
             || obj->type == OBJ_CLASS
             || obj->type == OBJ_CLOSURE
             || obj->type == OBJ_FUNCTION
             || obj->type == OBJ_INSTANCE
             || obj->type == OBJ_NATIVE
             || obj->type == OBJ_STRING
             || obj->type == OBJ_UPVALUE);
      return 0;
  }
}

size_t
klox_allocation_size(const struct cb *cb,
                     uint64_t         offset)
{
  cb_offset_t allocation_offset = PURE_OFFSET((cb_offset_t)offset);
  if (allocation_offset == CB_NULL)
    return 0;

  char *mem = (char *)cb_at(cb, allocation_offset);
  assert(alloc_is_object_get(mem));
  //if (!alloc_is_object_get(mem)) {
  //  return alloc_size_get(mem) + alloc_alignment_get(mem) - 1;
  //}
  return klox_Obj_external_size(cb, (Obj *)mem);
}

static int
klox_objtable_key_render(cb_offset_t           *dest_offset,
                         struct cb            **cb,
                         const struct cb_term  *term,
                         unsigned int           flags)
{
  // The key must be a u64 cb_term.
  assert(term->tag == CB_TERM_U64);

  return cb_asprintf(dest_offset, cb, "#%ju", (uintmax_t)cb_term_get_u64(term));
}

static int
klox_objtable_value_render(cb_offset_t           *dest_offset,
                           struct cb            **cb,
                           const struct cb_term  *term,
                           unsigned int           flags)
{
  // The value must be a u64 cb_term.
  assert(term->tag == CB_TERM_U64);

  cb_offset_t allocation_offset = (cb_offset_t)cb_term_get_u64(term);
  char *mem = (char *)cb_at(*cb, allocation_offset);

  assert(alloc_is_object_get(mem));

  return cb_asprintf(dest_offset, cb, "@%ju<s:%ju,a:%ju,ObjType:%d>",
                     (uintmax_t)allocation_offset,
                     (uintmax_t)alloc_size_get(mem),
                     (uintmax_t)alloc_alignment_get(mem),
                     (int)((Obj *)mem)->type);

}

int
objtablelayer_init(ObjTableLayer *layer) {
  structmap_init(&(layer->sm), &klox_allocation_size);
  layer->num_dense_entries = 0;
  layer->dense_external_size = 0;
  return 0;
}

int
objtablelayer_assign(ObjTableLayer *dest, ObjTableLayer *src) {
  *dest = *src;

  return 0;
}

int
objtablelayer_traverse(const struct cb                **cb,
                       ObjTableLayer                   *layer,
                       objtablelayer_traverse_func_t   func,
                       void                            *closure) {
  int ret;

  (void) ret;

  // Traverse the dense entries.
  for (unsigned int i = 0, e = layer->num_dense_entries; i < e; ++i) {
    uint64_t key = layer->dense[i];
    ObjTableLayerEntry *entry = objtablelayer_sparse_entry(layer, hash_key(key));
    uint64_t value = entry->value;

    assert(entry->n == i);

    func(key, value, closure);
  }

  // Traverse the structmap entries.
  ret = structmap_traverse(cb,
                           &(layer->sm),
                           (structmap_traverse_func_t)func,
                           closure);
  assert(ret == 0);

  return 0;
}

size_t
objtablelayer_external_size(ObjTableLayer *layer) {
  return structmap_external_size(&(layer->sm)) + layer->dense_external_size;
}

size_t
objtablelayer_internal_size(ObjTableLayer *layer) {
  return structmap_internal_size(&(layer->sm));
}

size_t
objtablelayer_size(ObjTableLayer *layer) {
  return structmap_size(&(layer->sm)) + layer->dense_external_size;
}

size_t
objtablelayer_modification_size(void)
{
  return structmap_modification_size();
}

void
objtablelayer_external_size_adjust(ObjTableLayer *layer,
                                   ssize_t        adjustment)
{
  //FIXME This should just adjust a layer-local variable
  structmap_external_size_adjust(&(layer->sm), adjustment);
}

int
objtablelayer_insert(struct cb        **cb,
                     struct cb_region  *region,
                     ObjTableLayer     *layer,
                     uint64_t           key,
                     uint64_t           value)
{
  unsigned int n = layer->num_dense_entries;

  if (n < OBJTABLELAYER_DENSE_SIZE) {
      uint64_t keyhash = hash_key(key);
      ObjTableLayerEntry *entry = objtablelayer_sparse_entry(layer, keyhash);

      bool already_used = entry->n < n && entry == objtablelayer_sparse_entry(layer, hash_key(layer->dense[entry->n]));
      if (!already_used) {
        layer->dense[n] = key;
        entry->n = n;
        entry->value = value;
        layer->num_dense_entries++;
        layer->dense_external_size += klox_allocation_size(*cb, value);
        return 0;
      }
  }

  return structmap_insert(cb, region, &(layer->sm), key, value);
}

int
methods_layer_init(struct cb **cb, struct cb_region *region, struct structmap *sm) {
  structmap_init(sm, &klox_no_external_size2);
  return 0;
}

int
fields_layer_init(struct cb **cb, struct cb_region *region, struct structmap *sm) {
  structmap_init(sm, &klox_no_external_size2);
  return 0;
}

extern inline void
objtable_cache_put(ObjTable        *obj_table,
                   uint64_t         key,
                   uint64_t         value)
{
  ObjTableSparseEntry *sparse_entry = objtable_sparse_entry(obj_table, hash_key(key));
  sparse_entry->key = key;
  sparse_entry->value = value;
}

void
objtable_cache_clear(ObjTable *obj_table)
{
  for (int i = 0; i < OBJTABLE_CACHE_SPARSE_SIZE; ++i)
    obj_table->sparse[i].key = 0;
}

void
objtable_init(ObjTable *obj_table)
{
  objtablelayer_init(&(obj_table->a));
  objtablelayer_init(&(obj_table->b));
  objtablelayer_init(&(obj_table->c));
  obj_table->next_obj_id.id  = 1;
  objtable_cache_clear(obj_table);
}

void
objtable_add_at(ObjTable *obj_table, ObjID obj_id, cb_offset_t offset)
{
  int ret;
  (void)ret;

  ret = objtablelayer_insert(&thread_cb, &thread_region, &(obj_table->a), obj_id.id, offset);
  assert(ret == 0);

  objtable_cache_put(obj_table, obj_id.id, offset);
}

ObjID
objtable_add(ObjTable *obj_table, cb_offset_t offset)
{
  ObjID obj_id = obj_table->next_obj_id;

  objtable_add_at(obj_table, obj_id, offset);
  (obj_table->next_obj_id.id)++;

  return obj_id;
}


cb_offset_t
objtable_lookup_uncached(ObjTable *obj_table, ObjID obj_id)
{
  uint64_t v;

  if (objtablelayer_lookup(thread_cb, &(obj_table->a), obj_id.id, &v) ||
      objtablelayer_lookup(thread_cb, &(obj_table->b), obj_id.id, &v) ||
      objtablelayer_lookup(thread_cb, &(obj_table->c), obj_id.id, &v)) {
    objtable_cache_put(obj_table, obj_id.id, v);
    return PURE_OFFSET((cb_offset_t)v);
  }

  return CB_NULL;
}

cb_offset_t
objtable_lookup_A(ObjTable *obj_table, ObjID obj_id)
{
  uint64_t v;

  if (objtablelayer_lookup(thread_cb, &(obj_table->a), obj_id.id, &v))
    return PURE_OFFSET((cb_offset_t)v);

  return CB_NULL;
}

cb_offset_t
objtable_lookup_B(ObjTable *obj_table, ObjID obj_id)
{
  uint64_t v;

  if (objtablelayer_lookup(thread_cb, &(obj_table->b), obj_id.id, &v))
    return PURE_OFFSET((cb_offset_t)v);

  return CB_NULL;
}

cb_offset_t
objtable_lookup_C(ObjTable *obj_table, ObjID obj_id)
{
  uint64_t v;

  if (objtablelayer_lookup(thread_cb, &(obj_table->c), obj_id.id, &v))
    return PURE_OFFSET((cb_offset_t)v);

  return CB_NULL;
}

void
objtable_invalidate(ObjTable *obj_table, ObjID obj_id)
{
  objtable_add_at(obj_table, obj_id, CB_NULL);
}

void
objtable_external_size_adjust_A(ObjTable *obj_table, ssize_t adjustment)
{
    objtablelayer_external_size_adjust(&(obj_table->a), adjustment);
}

void
objtable_freeze(ObjTable *obj_table)
{
  //assert(num_entries(obj_table->c) == 0); //FIXME create this check
  obj_table->c = obj_table->b;
  obj_table->b = obj_table->a;
  objtablelayer_init(&(obj_table->a));
}

size_t
objtable_consolidation_size(ObjTable *obj_table)
{
  size_t objtable_b_size = objtablelayer_size(&(obj_table->b));
  size_t objtable_c_size = objtablelayer_size(&(obj_table->c));
  KLOX_TRACE("objtable_b_size: %zu, objtable_c_size: %zu\n",
         objtable_b_size, objtable_c_size);
  return objtable_b_size + objtable_c_size + objtablelayer_modification_size();
}

cb_offset_t
resolveAsMutableLayer(ObjID objid)
{
  cb_offset_t o;

  assert(on_main_thread);
  assert(exec_phase == EXEC_PHASE_COMPILE || exec_phase == EXEC_PHASE_INTERPRET || exec_phase == EXEC_PHASE_FREE_WHITE_SET);

  o = objtable_lookup_A(&thread_objtable, objid);
  if (o != CB_NULL) {
    //KLOX_TRACE("#%ju@%ju found in objtable A\n", (uintmax_t)objid.id, (uintmax_t)o);
    assert(cb_offset_cmp(o, thread_cutoff_offset) > 0);
    return o;
  }

  o = objtable_lookup_B(&thread_objtable, objid);
  if (o != CB_NULL) {
    //KLOX_TRACE("#%ju@%ju found in objtable B\n", (uintmax_t)objid.id, (uintmax_t)o);
    cb_offset_t layer_o = deriveMutableObjectLayer(&thread_cb, &thread_region, objid, o);
    assert(cb_offset_cmp(layer_o, thread_cutoff_offset) > 0);
    objtable_add_at(&thread_objtable, objid, layer_o);
    //KLOX_TRACE("#%ju@%ju is new mutable layer in objtable A\n", (uintmax_t)objid_.id, layer_o);
    //KLOX_TRACE_ONLY(printObjectValue(OBJ_VAL(objid)));
    //KLOX_TRACE_(" is new mutable layer in objtable A\n");
    return layer_o;
  }

  o = objtable_lookup_C(&thread_objtable, objid);
  assert(o != CB_NULL);
  //KLOX_TRACE("#%ju@%ju found in objtable C\n", (uintmax_t)objid.id, (uintmax_t)o);
  cb_offset_t layer_o = deriveMutableObjectLayer(&thread_cb, &thread_region, objid, o);
  assert(cb_offset_cmp(layer_o, thread_cutoff_offset) > 0);
  objtable_add_at(&thread_objtable, objid, layer_o);
  //KLOX_TRACE("#%ju@%ju is new mutable layer in objtable A\n", (uintmax_t)objid_.id, layer_o);
  //KLOX_TRACE_ONLY(printObjectValue(OBJ_VAL(objid)));
  //KLOX_TRACE_(" is new mutable layer in objtable A\n");
  return layer_o;
}


static int
klox_object_cmp(Value lhs, Value rhs)
{
  ObjType lhsType = OBJ_TYPE(lhs);
  ObjType rhsType = OBJ_TYPE(rhs);

  if (lhsType < rhsType) return -1;
  if (lhsType > rhsType) return 1;

  switch (lhsType) {
    case OBJ_BOUND_METHOD:
    case OBJ_CLASS:
    case OBJ_CLOSURE:
    case OBJ_FUNCTION:
    case OBJ_INSTANCE:
    case OBJ_NATIVE:
    case OBJ_UPVALUE:
      if (lhs.val < rhs.val) return -1;
      if (lhs.val > rhs.val) return 1;
      return 0;

    case OBJ_STRING: {
      ObjString *lhsString = (ObjString *)AS_OBJ(lhs);
      ObjString *rhsString = (ObjString *)AS_OBJ(rhs);
      int shorterLength = lhsString->length < rhsString->length ? lhsString->length : rhsString->length;
      int ret;

      ret = memcmp(lhsString->chars.clp().cp(), rhsString->chars.clp().cp(), shorterLength);
      if (ret < 0) return -1;
      if (ret > 0) return 1;
      if (lhsString->length < rhsString->length) return -1;
      if (lhsString->length > rhsString->length) return 1;
      return 0;
    }

    default:
      assert(lhsType == OBJ_BOUND_METHOD
             || lhsType == OBJ_CLASS
             || lhsType == OBJ_CLOSURE
             || lhsType == OBJ_FUNCTION
             || lhsType == OBJ_INSTANCE
             || lhsType == OBJ_NATIVE
             || lhsType == OBJ_STRING
             || lhsType == OBJ_UPVALUE);
      return 0;
  }
}

//NOTE: This variant is suitable for comparing strings, pre-interning.
int
klox_value_deep_comparator(const struct cb *cb,
                           const struct cb_term *lhs,
                           const struct cb_term *rhs)
{
  // We expect to only use the double value of cb_terms.
  assert(lhs->tag == CB_TERM_DBL);
  assert(rhs->tag == CB_TERM_DBL);

  Value lhs_val = numToValue(cb_term_get_dbl(lhs));
  Value rhs_val = numToValue(cb_term_get_dbl(rhs));

  ValueType lhs_valtype = getValueType(lhs_val);
  ValueType rhs_valtype = getValueType(rhs_val);

  if (lhs_valtype < rhs_valtype) return -1;
  if (lhs_valtype > rhs_valtype) return 1;

  switch (lhs_valtype) {
    case VAL_BOOL:
      return (int)AS_BOOL(lhs_val) - (int)AS_BOOL(rhs_val);

    case VAL_NIL:
      //NOTE: All NILs are equal (of course).
      return 0;

    case VAL_NUMBER: {
      double lhs_num = AS_NUMBER(lhs_val);
      double rhs_num = AS_NUMBER(rhs_val);
      if (lhs_num < rhs_num) return -1;
      if (lhs_num > rhs_num) return 1;
      //NOTE: This comparator should rely on bitwise comparison when the doubles
      // are not less than and not greater than one another, just in case we are
      // in weird NaN territory.
      if (lhs_val.val < rhs_val.val) return -1;
      if (lhs_val.val > rhs_val.val) return 1;
      return 0;
    }

    case VAL_OBJ:
      return klox_object_cmp(lhs_val, rhs_val);

    default:
      assert(lhs_valtype == VAL_BOOL
             || lhs_valtype == VAL_NIL
             || lhs_valtype == VAL_NUMBER
             || lhs_valtype == VAL_OBJ);
      return 0;
  }
}


//NOTE: This variant is suitable for comparing strings, post-interning.
int
klox_value_shallow_comparator(const struct cb *cb,
                              const struct cb_term *lhs,
                              const struct cb_term *rhs)
{
  // We expect to only use the double value of cb_terms.
  assert(lhs->tag == CB_TERM_DBL);
  assert(rhs->tag == CB_TERM_DBL);

  Value lhsValue = numToValue(cb_term_get_dbl(lhs));
  Value rhsValue = numToValue(cb_term_get_dbl(rhs));

#ifndef NDEBUG
  // Check for string interning errors.
  if (OBJ_TYPE(lhsValue) == OBJ_STRING && OBJ_TYPE(rhsValue) == OBJ_STRING) {
      ObjString *lhsString = (ObjString *)AS_OBJ(lhsValue);
      ObjString *rhsString = (ObjString *)AS_OBJ(rhsValue);

      if (lhsString->length == rhsString->length
          && memcmp(lhsString->chars.clp().cp(), rhsString->chars.clp().cp(), lhsString->length) == 0
          && lhsValue.val != rhsValue.val) {
        fprintf(stderr, "String interning error detected! ObjString(%ju, %ju), \"%.*s\"(%ju, %ju)\n",
               (uintmax_t)lhsValue.val,
               (uintmax_t)rhsValue.val,
               lhsString->length,
               lhsString->chars.clp().cp(),
               (uintmax_t)lhsString->chars.co(),
               (uintmax_t)rhsString->chars.co());
        assert(lhsValue.val == rhsValue.val);
      }
  }
#endif //NDEBUG

  if (lhsValue.val < rhsValue.val) return -1;
  if (lhsValue.val > rhsValue.val) return 1;
  return 0;
}

static int
klox_object_render(cb_offset_t           *dest_offset,
                   struct cb            **cb,
                   const struct cb_term  *term,
                   unsigned int           flags)
{
  // We expect to only use the double value of cb_terms.
  assert(term->tag == CB_TERM_DBL);

  // We expect to only be used on object-type Values.
  Value value = numToValue(cb_term_get_dbl(term));
  assert(getValueType(value) == VAL_OBJ);

  ObjType objType = OBJ_TYPE(value);
  switch (objType) {
    case OBJ_BOUND_METHOD:
      return cb_asprintf(dest_offset, cb, "<bound_method@%ju>", (uintmax_t)AS_BOUND_METHOD_OID(value).id().id);
    case OBJ_CLASS:
      return cb_asprintf(dest_offset, cb, "<class@%ju>", (uintmax_t)AS_CLASS_OID(value).id().id);
    case OBJ_CLOSURE:
      return cb_asprintf(dest_offset, cb, "<closure@%ju>", (uintmax_t)AS_CLOSURE_OID(value).id().id);
    case OBJ_FUNCTION:
      return cb_asprintf(dest_offset, cb, "<fun@%ju>", (uintmax_t)AS_FUNCTION_OID(value).id().id);
    case OBJ_INSTANCE:
      return cb_asprintf(dest_offset, cb, "<instance@%ju>", (uintmax_t)AS_INSTANCE_OID(value).id().id);
    case OBJ_NATIVE:
      return cb_asprintf(dest_offset, cb, "<nativefun%p>", (void*)AS_NATIVE(value));
    case OBJ_STRING:{
      //CBINT FIXME this will be broken if the cb_asprintf() itself triggers a
      // resize and DEBUG_CLOBBER is on.  The source str will get overwritten
      // by the resize+clobber before it will be read to produce to target
      // string.
      const ObjString *str = AS_STRING_OID(value).clip().cp();

      if (str->length < 13) {
        return cb_asprintf(dest_offset, cb, "<string#%ju\"%.*s\"#%ju>",
            (uintmax_t)AS_STRING_OID(value).id().id,
            str->length,
            str->chars.clp(),
            (uintmax_t)str->chars.co());
      } else {
        return cb_asprintf(dest_offset, cb, "<string#%ju\"%.*s...%.*s\"%ju>",
            (uintmax_t)AS_STRING_OID(value).id().id,
            5,
            str->chars.clp().cp(),
            5,
            str->chars.clp().cp() + str->length - 5,
            (uintmax_t)str->chars.co());
      }
    }
    case OBJ_UPVALUE:
      return cb_asprintf(dest_offset, cb, "<upvalue@%ju>", (uintmax_t)AS_UPVALUE_OID(value).id().id);
    default:
      assert(objType == OBJ_BOUND_METHOD
             || objType == OBJ_CLASS
             || objType == OBJ_CLOSURE
             || objType == OBJ_FUNCTION
             || objType == OBJ_INSTANCE
             || objType == OBJ_NATIVE
             || objType == OBJ_STRING
             || objType == OBJ_UPVALUE);
      return 0;
  }
}

int
klox_value_render(cb_offset_t           *dest_offset,
                  struct cb            **cb,
                  const struct cb_term  *term,
                  unsigned int           flags)
{
  // We expect to only use the double value of cb_terms.
  assert(term->tag == CB_TERM_DBL);

  Value value = numToValue(cb_term_get_dbl(term));

  ValueType valType = getValueType(value);
  switch (valType) {
    case VAL_BOOL:
      return cb_asprintf(dest_offset, cb, "<%db>", (int)AS_BOOL(value));

    case VAL_NIL:
      return cb_asprintf(dest_offset, cb, "<nil>");

    case VAL_NUMBER:
      return cb_asprintf(dest_offset, cb, "<%ff>", AS_NUMBER(value));

    case VAL_OBJ:
      return klox_object_render(dest_offset, cb, term, flags);

    default:
      assert(valType == VAL_BOOL
             || valType == VAL_NIL
             || valType == VAL_NUMBER
             || valType == VAL_OBJ);
      return -1;
  }
}

void
klox_on_cb_resize(struct cb *old_cb, struct cb *new_cb)
{
  assert(on_main_thread);
  is_resizing = true;

  KLOX_TRACE("~~~~~~~~~~~~RESIZED from %ju to %ju (gc_outstanding? %d, old_cb: %p, new_cb: %p)~~~~~~~~~~~\n",
          (uintmax_t)cb_ring_size(old_cb), (uintmax_t)cb_ring_size(new_cb), gc_request_is_outstanding, old_cb, new_cb);

  tristack_recache(&(vm.tristack), new_cb);

  if (vm.currentFrame) {
    //If we're in some form of interpretation (having a currentFrame),  we need
    //to rewrite the internal pointers of each frame.

    for (unsigned int i = 0; i < vm.triframes.frameCount; ++i) {
      CallFrame *oldFrame = triframes_at_alt(&(vm.triframes), i, old_cb);
      CallFrame *newFrame = triframes_at_alt(&(vm.triframes), i, new_cb);

      if (!oldFrame->has_ip_offset) {
        assert(!newFrame->has_ip_offset);

        size_t old_ip_offset = oldFrame->ip - oldFrame->ip_root;
        newFrame->functionP = newFrame->closure.crip(new_cb).cp()->function.crip(new_cb).cp();
        newFrame->constantsValuesP = newFrame->functionP->chunk.constants.values.crp(new_cb).cp();
        newFrame->ip_root = newFrame->functionP->chunk.code.crp(new_cb).cp();
        newFrame->ip = newFrame->ip_root + old_ip_offset;
      }
    }

    triframes_recache(&(vm.triframes), new_cb);
    vm.currentFrame = vm.triframes.currentFrame;
    vm.currentFrame->slots = tristack_at(&(vm.tristack), vm.currentFrame->slotsIndex);

  } else {
    //If we're not in some form of interpretation (not having a currentFrame),
    //we nonetheless need to recache the direct pointers internal to the
    //TriFrames struct (in particular the adirect one).
    triframes_recache(&(vm.triframes), new_cb);
  }

  // Rewrite all rewritable pointers (generally held on C-stack frames).
  rcbp_rewrite_list(new_cb);

#if KLOX_SYNC_GC
  // If we're in the synchronous GC mode, we can optionally clobber old
  // contents from the old cb for increased rigor.  If we're not in this mode,
  // the GC thread may still be using those contents, so we cannot.
#ifdef DEBUG_CLOBBER
#ifdef DEBUG_TRACE_GC
  KLOX_TRACE("clobbering range [%ju,%ju) of old CB %p\n",
             (uintmax_t)cb_start(old_cb),
             (uintmax_t)(cb_start(old_cb) + cb_data_size(old_cb)),
             old_cb);
#endif  //DEBUG_TRACE_GC
  cb_memset(old_cb, cb_start(old_cb), '&', cb_data_size(old_cb));
#endif //DEBUG_CLOBBER
#endif //KLOX_SYNC_GC

  thread_ring_start = cb_ring_start(new_cb);
  thread_ring_mask = cb_ring_mask(new_cb);
  is_resizing = false;

  KLOX_TRACE("~~~~~RESIZE COMPLETE~~~~~\n");
}

void
gc_submit_request(struct gc_request_response *rr) {
  KLOX_TRACE("Submitting GC request %p  (gc_last_processed_response:%p)\n", rr, gc_last_processed_response);
  gc_current_request.store(rr, std::memory_order_release);
  KLOX_TRACE("Submitted GC request %p (gc_last_processed_response:%p)\n", rr, gc_last_processed_response);
  gc_request_is_outstanding = true;
}

static void
gc_submit_response(struct gc_request_response *rr) {
  KLOX_TRACE("Submitting GC response %p\n", rr);
  gc_current_response.store(rr, std::memory_order_release);
  KLOX_TRACE("Submitted GC response %p\n", rr);
}

struct gc_request_response *
gc_await_response(void) {
  struct gc_request_response *rr;

  KLOX_TRACE("Awaiting GC response (gc_last_processed_response:%p)\n", gc_last_processed_response);
  do {
    rr = gc_current_response.load(std::memory_order_acquire);
  } while (rr == gc_last_processed_response);
  KLOX_TRACE("Received GC response %p\n", rr);

  // We have at most one outstanding GC request.
  assert(rr == gc_current_request.load(std::memory_order_relaxed));

  return rr;
}

void
integrate_any_gc_response(void) {
  struct gc_request_response *rr1, *rr2;

  // Sample in lightweight way because we may not have a response.
  rr1 = gc_current_response.load(std::memory_order_relaxed);
  if (rr1 == gc_last_processed_response)
    return;

  // Sample again but with proper memory barrier, because we have a response.
  rr2 = gc_current_response.load(std::memory_order_acquire);
  assert(rr2 == rr1);

  // We have at most one outstanding GC request.
  assert(rr2 == gc_current_request.load(std::memory_order_relaxed));

  KLOX_TRACE("Received on main thread a newer gc_request_response (%p) than our last one (%p)\n",
             rr1, gc_last_processed_response);

  integrateGCResponse(rr2);

  KLOX_TRACE("setting gc_last_processed_response to %p\n", rr2);
  gc_last_processed_response = rr2;
}


void
gc_main_loop(void)
{
  //printf("DANDEBUG On GC thread\n");
  struct gc_request_response *last_request = 0;
  struct gc_request_response *curr_request;
  int ret;

  while (!gc_stop_flag.load(std::memory_order_relaxed)) {
    curr_request = gc_current_request.load(std::memory_order_acquire);
    if (curr_request == last_request) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

#if KLOX_SYNC_GC
    can_print = true;
#endif  //KLOX_SYNC_GC

    //printf("DANDEBUG Received new GC request %p\n", curr_request);

    //Do the Garbage Collection / Condensing.
    // Make the threadlocal cb the target cb.
    thread_cb = curr_request->req.orig_cb;
    thread_ring_start = cb_ring_start(thread_cb);
    thread_ring_mask  = cb_ring_mask(thread_cb);
    // Make the threadlocal ObjTable resolve toward the target's frozen B and C layers.
    objtablelayer_init(&(thread_objtable.a));
    objtablelayer_assign(&(thread_objtable.b), &(curr_request->req.objtable_b));
    objtablelayer_assign(&(thread_objtable.c), &(curr_request->req.objtable_c));
    objtable_cache_clear(&thread_objtable);
    ret = gc_perform(curr_request);
    if (ret != 0) {
      fprintf(stderr, "Failed to GC via CB.\n");
    }
    assert(ret == 0);


#if KLOX_SYNC_GC
    can_print = false;
#endif  //KLOX_SYNC_GC

    //printf("DANDEBUG Responding to GC request %p\n", curr_request);
    gc_submit_response(curr_request);
    //printf("DANDEBUG Responded to GC request %p\n", curr_request);

    last_request = curr_request;
  }

  //printf("DANDEBUG Exiting GC thread\n");
}

int
gc_init(void)
{
  gc.grayCount = 0;
  gc.grayCapacity = 0;
  gc.grayStack = CB_NULL;

  struct cb_params cb_params = CB_PARAMS_DEFAULT;

  // Create a cb for the GC to use.
  cb_params.ring_size = 8388608 * 4;
  cb_params.mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;
  cb_params.flags |= CB_PARAMS_F_MLOCK;
  cb_params.on_resize = &klox_on_cb_resize;
  strncpy(cb_params.filename_prefix, "gc", sizeof(cb_params.filename_prefix));
  gc_thread_cb = cb_create(&cb_params, sizeof(cb_params));
  if (!gc_thread_cb) {
    fprintf(stderr, "Could not create GC's scratch continuous buffer. \n");
    return -1;
  }

  // Spawn the GC thread.
  gc_thread = std::thread(gc_main_loop);

  return 0;
}


int
gc_deinit(void)
{
  // Cause the GC thread to terminate.
  gc_stop_flag.store(true, std::memory_order_relaxed);
  gc_thread.join();
  //printf("DANDEBUG GC thread rejoined\n");
  return 0;
}


struct merge_class_methods_closure
{
  struct cb         *src_cb;
  struct structmap  *b_class_methods_sm;
  struct cb         *dest_cb;
  struct cb_region  *dest_region;
  struct structmap  *dest_methods_sm;
  DEBUG_ONLY(size_t  last_sm_size);
};

static int
merge_c_class_methods(uint64_t  k,
                      uint64_t  v,
                      void     *closure)
{
  struct merge_class_methods_closure *cl = (struct merge_class_methods_closure *)closure;
  int ret;

  (void)ret;

  // The presence of an entry under this method name in the B class masks
  // our value, so no sense in copying it.
  if (structmap_contains_key(cl->src_cb, cl->b_class_methods_sm, k))
    return 0;

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));

  ret = structmap_insert(&(cl->dest_cb),
                         cl->dest_region,
                         cl->dest_methods_sm,
                         k,
                         v);
  assert(ret == 0);
  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));
  DEBUG_ONLY(size_t      sm_size = structmap_size(cl->dest_methods_sm));

  // Actual bytes used must be <= structmap's self-considered growth.
  assert(c1 - c0 <= sm_size - cl->last_sm_size);
  DEBUG_ONLY(cl->last_sm_size = sm_size);

  return 0;
}

struct merge_instance_fields_closure
{
  struct cb         *src_cb;
  struct structmap  *b_instance_fields_sm;
  struct cb         *dest_cb;
  struct cb_region  *dest_region;
  struct structmap  *dest_fields_sm;
  DEBUG_ONLY(size_t  last_sm_size);
};

static int
merge_c_instance_fields(uint64_t  k,
                        uint64_t  v,
                        void     *closure)
{
  struct merge_instance_fields_closure *cl = (struct merge_instance_fields_closure *)closure;
  int ret;

  (void)ret;

  // The presence of an entry under this method name in the B class masks
  // our value, so no sense in copying it.
  if (structmap_contains_key(cl->src_cb, cl->b_instance_fields_sm, k))
    return 0;

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));

  ret = structmap_insert(&(cl->dest_cb),
                         cl->dest_region,
                         cl->dest_fields_sm,
                         k,
                         v);
  assert(ret == 0);
  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));
  DEBUG_ONLY(size_t      sm_size = structmap_size(cl->dest_fields_sm));

  // Actual bytes used must be <= structmap's self-considered growth.
  assert(c1 - c0 <= sm_size - cl->last_sm_size);
  DEBUG_ONLY(cl->last_sm_size = sm_size);

  return 0;
}

struct copy_objtable_closure
{
  struct cb        *src_cb;
  struct cb        *dest_cb;
  struct cb_region *dest_region;
  ObjTableLayer    *new_b;
  DEBUG_ONLY(size_t last_new_b_external_size);
  DEBUG_ONLY(size_t last_new_b_internal_size);
  DEBUG_ONLY(size_t last_new_b_size);
  ObjID             white_list;
};

static int
copy_objtable_b(uint64_t  key,
                uint64_t  val,
                void     *closure)
{
  //NOTE: This is generating a condensed #ID -> @offset mapping into an
  // initially-blank ObjTableLayer.  Each of the new entries contain the same
  // old #ID, but with a new @offset which points to a clone of the object which
  // existed at the old @offset. In the end, the ObjTableLayer contents are the
  // same value-wise, but have condensed relocations.

  struct copy_objtable_closure *cl = (struct copy_objtable_closure *)closure;
  ObjID obj_id = { .id = key };
  cb_offset_t offset = (cb_offset_t)val;
  bool newly_white = false;
  cb_offset_t clone_offset;
  int ret;

  assert(!ALREADY_WHITE(offset));

  //Skip those ObjIDs which have been invalidated.  (CB_NULL serves as a
  // tombstone in such cases).
  if (offset == CB_NULL) {
    KLOX_TRACE("skipping invalidated object #%ju.\n", (uintmax_t)obj_id.id);
    return 0;
  }

  //Skip those ObjIDs which are not marked dark (and are therefore unreachable
  // from the roots of the VM state).
  if (!objectIsDark(obj_id)) {
    KLOX_TRACE("preserving newly white object #%ju.\n", (uintmax_t)obj_id.id);
    newly_white = true;
  }

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));

  clone_offset = cloneObject(&(cl->dest_cb), cl->dest_region, obj_id, offset);

  DEBUG_ONLY(cb_offset_t c0a = cb_region_cursor(cl->dest_region));

  if (newly_white) {
    CBO<Obj> clonedObj = clone_offset;
    clonedObj.mrp(cl->dest_cb).mp()->white_next = cl->white_list;
    cl->white_list = obj_id;
  }

  DEBUG_ONLY(cb_region_align_cursor(&(cl->dest_cb), cl->dest_region, objtablelayer_insertion_alignment_get()));

  DEBUG_ONLY(cb_offset_t c0b = cb_region_cursor(cl->dest_region));

  ret = objtablelayer_insert(&(cl->dest_cb),
                             cl->dest_region,
                             cl->new_b,
                             key,
                             (uint64_t)(clone_offset | (newly_white ? ALREADY_WHITE_FLAG : 0)));
  assert(ret == 0);

  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));

  DEBUG_ONLY(size_t external_used_bytes = (size_t)(c0a - c0));
  DEBUG_ONLY(size_t internal_used_bytes = (size_t)(c1 - c0b));
  DEBUG_ONLY(size_t total_used_bytes = (size_t)(c1 - c0));
  DEBUG_ONLY(size_t new_b_external_size = objtablelayer_external_size(cl->new_b));
  DEBUG_ONLY(size_t new_b_internal_size = objtablelayer_internal_size(cl->new_b));
  DEBUG_ONLY(size_t new_b_size = objtablelayer_size(cl->new_b));
  DEBUG_ONLY(KLOX_TRACE("+%ju external, +%ju internal bytes (external estimate:+%ju, internal estimate:+%ju) #%ju -> @%ju %s\n",
         (uintmax_t)external_used_bytes,
         (uintmax_t)internal_used_bytes,
         (uintmax_t)(new_b_external_size - cl->last_new_b_external_size),
         (uintmax_t)(new_b_internal_size - cl->last_new_b_internal_size),
         (uintmax_t)obj_id.id,
         (uintmax_t)clone_offset,
         (newly_white ? "NEWLYWHITE" : "")));

  // Actual bytes used must be <= BST's self-considered growth.
  assert(external_used_bytes <= klox_Obj_external_size(cl->dest_cb, (Obj*)cb_at(cl->src_cb, offset)));
  assert(external_used_bytes <= new_b_external_size - cl->last_new_b_external_size);
  assert(internal_used_bytes <= new_b_internal_size - cl->last_new_b_internal_size);
  assert(total_used_bytes <= new_b_size - cl->last_new_b_size);
  DEBUG_ONLY(cl->last_new_b_external_size = new_b_external_size);
  DEBUG_ONLY(cl->last_new_b_internal_size = new_b_internal_size);
  DEBUG_ONLY(cl->last_new_b_size = new_b_size);

  (void)ret;
  return 0;
}

static int
copy_objtable_c_not_in_b(uint64_t  key,
                         uint64_t  val,
                         void     *closure)
{
  //NOTE: For #ObjID keys which do not exist in B, this is simply copying the
  // #ObjID -> @offset mapping into a cb_bst which already contains the entries
  // from B. However, for #ObjID keys which DO exist in B and which are for Objs
  // which have internal maps of their own (ObjClass's methods, and
  // ObjInstance's fields), a new Obj must be created to contain the merged set
  // of these contents.

  struct copy_objtable_closure *cl = (struct copy_objtable_closure *)closure;
  OID<Obj> objOID = (ObjID) { .id = key };
  cb_offset_t cEntryOffset = PURE_OFFSET((cb_offset_t)val);
  bool already_white = ALREADY_WHITE((cb_offset_t)val);
  bool newly_white = false;
  uint64_t temp_val = 0;
  ssize_t external_size_adjustment = 0;
  int ret;

  //Region C should never contain invalidated entries.
  assert(cEntryOffset != CB_NULL);

  //Skip those ObjIDs which are not marked dark (and are therefore unreachable
  // from the roots of the VM state).
  if (!objectIsDark(objOID)) {
    if (already_white) {
      KLOX_TRACE("skipping already white object #%ju.\n", (uintmax_t)objOID.id().id);
      return 0;
    } else {
      KLOX_TRACE("preserving newly white object #%ju.\n", (uintmax_t)objOID.id().id);
      newly_white = true;
    }
  }

  DEBUG_ONLY(size_t external_used_bytes = 0);
  DEBUG_ONLY(size_t internal_used_bytes = 0);
  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));

  // If an entry exists in both B and C, B's entry should mask C's EXCEPT when
  // the B entry and C entry can be merged (which is when they are both ObjClass
  // or both ObjInstance objects).
  if (objtablelayer_lookup(cl->src_cb, cl->new_b, key, &temp_val) == true) {
    cb_offset_t bEntryOffset = (cb_offset_t)temp_val;
    CBO<Obj> bEntryObj = bEntryOffset;
    CBO<Obj> cEntryObj = cEntryOffset;

    if (bEntryObj.clp().cp()->type == OBJ_CLASS && cEntryObj.clp().cp()->type == OBJ_CLASS) {
      //Copy C ObjClass's methods WHICH DO NOT EXIST IN B ObjClass's methods
      //into the B ObjClass's methods set.
      ObjClass *classB = (ObjClass *)bEntryObj.mlp().mp();  //cb-resize-safe (GC preallocated space)
      ObjClass *classC = (ObjClass *)cEntryObj.clp().cp();  //cb-resize-safe (GC preallocated space)
      struct merge_class_methods_closure subclosure;
      size_t old_sm_size = structmap_size(&(classB->methods_sm));
      size_t new_sm_size;

      subclosure.src_cb              = cl->src_cb;
      subclosure.b_class_methods_sm  = &(classB->methods_sm);
      subclosure.dest_cb             = cl->dest_cb;
      subclosure.dest_region         = cl->dest_region;
      subclosure.dest_methods_sm     = &(classB->methods_sm);
      DEBUG_ONLY(subclosure.last_sm_size = structmap_size(&(classB->methods_sm)));

      ret = structmap_traverse((const struct cb **)&(cl->src_cb),
                               &(classC->methods_sm),
                               merge_c_class_methods,
                               &subclosure);
      assert(ret == 0);

      new_sm_size = structmap_size(&(classB->methods_sm));

      external_size_adjustment = (ssize_t)new_sm_size - (ssize_t)old_sm_size;
      DEBUG_ONLY(external_used_bytes = external_size_adjustment);
    } else if (bEntryObj.clp().cp()->type == OBJ_INSTANCE && cEntryObj.clp().cp()->type == OBJ_INSTANCE) {
      //Copy C ObjInstance's fields WHICH DO NOT EXIST IN B ObjInstance's
      //fields into the B ObjInstance's fields set.
      ObjInstance *instanceB = (ObjInstance *)bEntryObj.mlp().mp();  //cb-resize-safe (GC preallocated space)
      ObjInstance *instanceC = (ObjInstance *)cEntryObj.clp().cp();  //cb-resize-safe (GC preallocated space)
      struct merge_instance_fields_closure subclosure;
      size_t old_sm_size = structmap_size(&(instanceB->fields_sm));
      size_t new_sm_size;

      subclosure.src_cb                = cl->src_cb;
      subclosure.b_instance_fields_sm  = &(instanceB->fields_sm);
      subclosure.dest_cb               = cl->dest_cb;
      subclosure.dest_region           = cl->dest_region;
      subclosure.dest_fields_sm        = &(instanceB->fields_sm);
      DEBUG_ONLY(subclosure.last_sm_size = structmap_size(&(instanceB->fields_sm)));

      ret = structmap_traverse((const struct cb **)&(cl->src_cb),
                               &(instanceC->fields_sm),
                               merge_c_instance_fields,
                               &subclosure);
      assert(ret == 0);

      new_sm_size = structmap_size(&(instanceB->fields_sm));

      external_size_adjustment = (ssize_t)new_sm_size - (ssize_t)old_sm_size;
      DEBUG_ONLY(external_used_bytes = external_size_adjustment);
    } else {
      // B's entry masks C's, so skip C's entry.
      // (We are presently traversing C's entries.)
      return 0;
    }
  } else {
    //Nothing in B masks the presently-traversed entry in C, just insert
    //a clone of it.
    cb_offset_t clone_offset = cloneObject(&(cl->dest_cb), cl->dest_region, objOID.id(), cEntryOffset);

    DEBUG_ONLY(cb_offset_t c0a = cb_region_cursor(cl->dest_region));

    DEBUG_ONLY(external_used_bytes = (size_t)(c0a - c0));

    if (newly_white) {
      CBO<Obj> clonedObj = clone_offset;
      clonedObj.mrp(cl->dest_cb).mp()->white_next = cl->white_list;
      cl->white_list = objOID.id();
    }

    DEBUG_ONLY(cb_region_align_cursor(&(cl->dest_cb), cl->dest_region, objtablelayer_insertion_alignment_get()));

    DEBUG_ONLY(cb_offset_t c0b = cb_region_cursor(cl->dest_region));

    ret = objtablelayer_insert(&(cl->dest_cb),
                               cl->dest_region,
                               cl->new_b,
                               key,
                               (uint64_t)(clone_offset | (newly_white ? ALREADY_WHITE_FLAG : 0)));
    assert(ret == 0);

    DEBUG_ONLY(internal_used_bytes = (size_t)(cb_region_cursor(cl->dest_region) - c0b));
  }

  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));

  //NOTE: When we have expanded the size of the ObjClass or ObjInstance which
  // had already existed as inserted in new_root_b, we have to adjust
  // new_root_b's notion of its external size.
  if (external_size_adjustment != 0) {
    assert(external_size_adjustment > 0); //We only add things.
    objtablelayer_external_size_adjust(cl->new_b, external_size_adjustment);
  }

  DEBUG_ONLY(size_t total_used_bytes = (size_t)(c1 - c0));
  DEBUG_ONLY(size_t new_b_external_size = objtablelayer_external_size(cl->new_b));
  DEBUG_ONLY(size_t new_b_internal_size = objtablelayer_internal_size(cl->new_b));
  DEBUG_ONLY(size_t new_b_size = objtablelayer_size(cl->new_b));
  DEBUG_ONLY(KLOX_TRACE("+%ju external, +%ju internal bytes (external estimate:+%ju, internal estimate +%ju) #%ju -> @%ju (external_size_adjustment: %zd)\n",
         (uintmax_t)external_used_bytes,
         (uintmax_t)internal_used_bytes,
         (uintmax_t)(new_b_external_size - cl->last_new_b_external_size),
         (uintmax_t)(new_b_internal_size - cl->last_new_b_internal_size),
         (uintmax_t)objOID.id().id,
         (uintmax_t)cEntryOffset,
         external_size_adjustment));

  // Actual bytes used must be <= BST's self-considered growth.
  assert(external_used_bytes <= klox_Obj_external_size(cl->dest_cb, (Obj*)cb_at(cl->src_cb, cEntryOffset)));
  assert(external_used_bytes <= new_b_external_size - cl->last_new_b_external_size);
  assert(internal_used_bytes <= new_b_internal_size - cl->last_new_b_internal_size);
  assert(total_used_bytes <= new_b_size - cl->last_new_b_size);
  DEBUG_ONLY(cl->last_new_b_external_size = new_b_external_size);
  DEBUG_ONLY(cl->last_new_b_internal_size = new_b_internal_size);
  DEBUG_ONLY(cl->last_new_b_size = new_b_size);

  (void)ret;
  return 0;
}


struct copy_strings_closure
{
  struct cb        *src_cb;
  cb_offset_t       old_root_b;
  cb_offset_t       old_root_c;
  struct cb        *dest_cb;
  struct cb_region *dest_region;
  cb_offset_t      *new_root_b;
  DEBUG_ONLY(size_t last_bst_size);
};

static int
copy_strings_b(const struct cb_term *key_term,
               const struct cb_term *value_term,
               void                 *closure)
{
  struct copy_strings_closure *cl = (struct copy_strings_closure *)closure;
  Value keyValue = numToValue(cb_term_get_dbl(key_term));
  int ret;

  //Skip those ObjIDs which are not marked dark (and are therefore unreachable
  // from the roots of the VM state).
  if (!objectIsDark(AS_OBJ_ID(keyValue))) {
    KLOX_TRACE("dropping unreachable string ");
    KLOX_TRACE_ONLY(printValue(keyValue, false));
    KLOX_TRACE_("\n");
    return 0;
  }

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));
  ret = cb_bst_insert(&(cl->dest_cb),
                      cl->dest_region,
                      cl->new_root_b,
                      cb_region_start(cl->dest_region),  //NOTE: full contents are mutable
                      key_term,
                      value_term);
  assert(ret == 0);
  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));
  DEBUG_ONLY(size_t      bst_size = cb_bst_size(cl->dest_cb, *cl->new_root_b));
  DEBUG_ONLY(KLOX_TRACE("+%ju bytes (growth:+%ju)\n",
         (uintmax_t)(c1 - c0),
         (uintmax_t)(bst_size - cl->last_bst_size)));

  // Actual bytes used must be <= BST's self-considered growth.
  assert(c1 - c0 <= bst_size - cl->last_bst_size);
  DEBUG_ONLY(cl->last_bst_size = bst_size);

  (void)ret;
  return 0;
}

static int
copy_strings_c_not_in_b(const struct cb_term *key_term,
                        const struct cb_term *value_term,
                        void                 *closure)
{
  struct copy_strings_closure *cl = (struct copy_strings_closure *)closure;
  Value keyValue = numToValue(cb_term_get_dbl(key_term));
  struct cb_term temp_term;
  int ret;

  (void)ret;

  //Skip those ObjIDs which are not marked dark (and are therefore unreachable
  // from the roots of the VM state).
  if (!objectIsDark(AS_OBJ_ID(keyValue))) {
    KLOX_TRACE("dropping unreachable string ");
    KLOX_TRACE_ONLY(printValue(keyValue, false));
    KLOX_TRACE_("\n");
    return 0;
  }


  // If an entry exists in both B and C, B's entry should mask C's.
  if (cb_bst_lookup(cl->src_cb, cl->old_root_b, key_term, &temp_term) == 0)
      return 0;

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));
  ret = cb_bst_insert(&(cl->dest_cb),
                      cl->dest_region,
                      cl->new_root_b,
                      cb_region_start(cl->dest_region),  //NOTE: full contents are mutable
                      key_term,
                      value_term);
  assert(ret == 0);
  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));
  DEBUG_ONLY(size_t      bst_size = cb_bst_size(cl->dest_cb, *cl->new_root_b));
  DEBUG_ONLY(KLOX_TRACE("+%ju bytes (growth:+%ju)\n",
         (uintmax_t)(c1 - c0),
         (uintmax_t)(bst_size - cl->last_bst_size)));

  // Actual bytes used must be <= BST's self-considered growth.
  assert(c1 - c0 <= bst_size - cl->last_bst_size);
  DEBUG_ONLY(cl->last_bst_size = bst_size);

  return 0;
}


struct copy_globals_closure
{
  struct cb        *src_cb;
  cb_offset_t       old_root_b;
  cb_offset_t       old_root_c;
  struct cb        *dest_cb;
  struct cb_region *dest_region;
  cb_offset_t      *new_root_b;
  DEBUG_ONLY(size_t last_bst_size);
};

static int
copy_globals_b(const struct cb_term *key_term,
               const struct cb_term *value_term,
               void                 *closure)
{
  struct copy_globals_closure *cl = (struct copy_globals_closure *)closure;
  int ret;

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));
  ret = cb_bst_insert(&(cl->dest_cb),
                      cl->dest_region,
                      cl->new_root_b,
                      cb_region_start(cl->dest_region),  //NOTE: full contents are mutable
                      key_term,
                      value_term);
  assert(ret == 0);
  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));
  DEBUG_ONLY(size_t      bst_size = cb_bst_size(cl->dest_cb, *cl->new_root_b));
  DEBUG_ONLY(KLOX_TRACE("+%ju bytes (growth:+%ju)\n",
         (uintmax_t)(c1 - c0),
         (uintmax_t)(bst_size - cl->last_bst_size)));

  // Actual bytes used must be <= BST's self-considered growth.
  assert(c1 - c0 <= bst_size - cl->last_bst_size);
  DEBUG_ONLY(cl->last_bst_size = bst_size);

  (void)ret;
  return 0;
}

static int
copy_globals_c_not_in_b(const struct cb_term *key_term,
                        const struct cb_term *value_term,
                        void                 *closure)
{
  struct copy_globals_closure *cl = (struct copy_globals_closure *)closure;
  struct cb_term temp_term;
  int ret;

  (void)ret;

  // If an entry exists in both B and C, B's entry should mask C's.
  if (cb_bst_lookup(cl->src_cb, cl->old_root_b, key_term, &temp_term) == 0)
      return 0;

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));
  ret = cb_bst_insert(&(cl->dest_cb),
                      cl->dest_region,
                      cl->new_root_b,
                      cb_region_start(cl->dest_region),  //NOTE: full contents are mutable
                      key_term,
                      value_term);
  assert(ret == 0);
  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));
  DEBUG_ONLY(size_t      bst_size = cb_bst_size(cl->dest_cb, *cl->new_root_b));
  DEBUG_ONLY(KLOX_TRACE("+%ju bytes (growth:+%ju)\n",
         (uintmax_t)(c1 - c0),
         (uintmax_t)(bst_size - cl->last_bst_size)));

  // Actual bytes used must be <= BST's self-considered growth.
  assert(c1 - c0 <= bst_size - cl->last_bst_size);
  DEBUG_ONLY(cl->last_bst_size = bst_size);

  return 0;
}

static int
grayObjtableTraversal(uint64_t key, uint64_t value, void *closure)
{
  const char *desc = (const char *)closure;
  ObjID objID = { .id = key };

  (void)desc;

  KLOX_TRACE("%s graying #%ju\n",
             desc,
             (uintmax_t)objID.id);

  grayObject(objID);

  return 0;
}

int
gc_perform(struct gc_request_response *rr)
{
  int ret;

  gc_phase = GC_PHASE_RESET_GC_STATE;
  cb_rewind_to(gc_thread_cb, 0);
  KLOX_TRACE("before GC region allocation\n");
  ret = logged_region_create(&gc_thread_cb, &gc_thread_region, 1, 1024, 0);
  KLOX_TRACE("after GC region allocation\n");
  if (ret != CB_SUCCESS)
  {
      fprintf(stderr, "Could not create GC region.\n");
      assert(ret == CB_SUCCESS);
      exit(EXIT_FAILURE);  //FIXME _exit()?
  }
  gc.grayCount = 0;
  gc.grayCapacity = 0;
  gc.grayStack = CB_NULL;
  clearDarkObjectSet();

  //NOTE: The compilation often hold objects in stack-based (the C language
  // stack, not the vm.stack) temporaries which are invisble to the garbage
  // collector.  Finding all code paths where multiple allocations happen and
  // ensuring that the earlier allocations wind up temporarily on the vm.stack
  // so as not to be freed by the garbage collector is difficult.  This
  // alternative is to just consider all objtable objects as reachable until
  // we get out of the compilation phase.  The A layer is empty, so not
  // traversed here.
  if (rr->req.exec_phase == EXEC_PHASE_COMPILE) {
    int ret;

    (void) ret;

    ret = objtablelayer_traverse((const struct cb **)&(rr->req.orig_cb),
                                 &(rr->req.objtable_b),
                                 &grayObjtableTraversal,
                                 (void*)"B");
    assert(ret == 0);
    ret = objtablelayer_traverse((const struct cb **)&(rr->req.orig_cb),
                                 &(rr->req.objtable_c),
                                 &grayObjtableTraversal,
                                 (void*)"C");
    assert(ret == 0);
  }

  gc_phase = GC_PHASE_MARK_STACK_ROOTS;
  {
    TriStack ts;
    ts.abo        = 0;
    ts.abi        = rr->req.tristack_abi;
    ts.bbo        = rr->req.tristack_bbo;
    ts.bbi        = rr->req.tristack_bbi;
    ts.cbo        = rr->req.tristack_cbo;
    ts.cbi        = rr->req.tristack_cbi;
    ts.stackDepth = rr->req.tristack_stackDepth;
    tristack_recache(&ts, rr->req.orig_cb);

    for (unsigned int i = 0; i < ts.stackDepth; ++i) {
      grayValue(*tristack_at(&ts, i));
    }
  }

  gc_phase = GC_PHASE_MARK_FRAMES_ROOTS;
  {
    TriFrames tf;
    tf.abo        = 0;
    tf.abi        = rr->req.triframes_abi;
    tf.bbo        = rr->req.triframes_bbo;
    tf.bbi        = rr->req.triframes_bbi;
    tf.cbo        = rr->req.triframes_cbo;
    tf.cbi        = rr->req.triframes_cbi;
    tf.frameCount = rr->req.triframes_frameCount;
    triframes_recache(&tf, rr->req.orig_cb);

    for (unsigned int i = 0; i < tf.frameCount; i++) {
      grayObject(triframes_at(&tf, i)->closure.id());
    }
  }

  gc_phase = GC_PHASE_MARK_OPEN_UPVALUES;
  for (OID<ObjUpvalue> upvalue = rr->req.open_upvalues;
       !upvalue.is_nil();
       upvalue = upvalue.clip().cp()->next) {
    grayObject(upvalue.id());
  }

  //NOTE: vm.strings is omitted here because it only holds weak references.
  // These entries will be removed during consolidation if they were not
  // grayed as reachable from the root set.
  gc_phase = GC_PHASE_MARK_GLOBAL_ROOTS;
  {
    Table globals;
    globals.root_a = CB_BST_SENTINEL;
    globals.root_b = rr->req.globals_root_b;
    globals.root_c = rr->req.globals_root_c;
    grayTable(&globals);
  }
  grayCompilerRoots();
  grayObject(rr->req.init_string);

  // Traverse the references.
  gc_phase = GC_PHASE_MARK_ALL_LEAVES;
  while (gc.grayCount > 0) {
    // Pop an item from the gray stack.
    OID<Obj> object = gc.grayStack.crp(gc_thread_cb).cp()[--gc.grayCount];
    grayObjectLeaves(object);
  }

  gc_phase = GC_PHASE_CONSOLIDATE;

  // Condense objtable
  {
    struct copy_objtable_closure closure;

    KLOX_TRACE("condense objtable 0:  orig_cb:%p  dest_region:[s:%ju,c:%ju,e:%ju)\n",
        rr->req.orig_cb,
        cb_region_start(&rr->req.objtable_new_region),
        cb_region_cursor(&rr->req.objtable_new_region),
        cb_region_end(&rr->req.objtable_new_region));

    objtablelayer_init(&(rr->resp.objtable_new_b));
    KLOX_TRACE("condense objtable 1:  new_root_b: %ju\n", (uintmax_t)rr->resp.objtable_new_b.sm.root_node_offset);

    closure.src_cb      = rr->req.orig_cb;
    closure.dest_cb     = rr->req.orig_cb;
    closure.dest_region = &(rr->req.objtable_new_region);
    closure.new_b       = &(rr->resp.objtable_new_b);
    DEBUG_ONLY(closure.last_new_b_external_size = objtablelayer_external_size(&(rr->resp.objtable_new_b)));
    DEBUG_ONLY(closure.last_new_b_internal_size = objtablelayer_internal_size(&(rr->resp.objtable_new_b)));
    DEBUG_ONLY(closure.last_new_b_size = objtablelayer_size(&(rr->resp.objtable_new_b)));
    closure.white_list  = CB_NULL_OID;

    KLOX_TRACE("condense objtable 2:  new_root_b: %ju\n", (uintmax_t)rr->resp.objtable_new_b.sm.root_node_offset);

    ret = objtablelayer_traverse((const struct cb **)&(rr->req.orig_cb),
                                 &(rr->req.objtable_b),
                                 copy_objtable_b,
                                 &closure);
    assert(ret == 0);
    KLOX_TRACE("condense objtable 3:  new_root_b: %ju\n", (uintmax_t)rr->resp.objtable_new_b.sm.root_node_offset);
    KLOX_TRACE("done with copy_objtable_b(). region: [s:%ju, c:%ju, e:%ju], used size: %ju\n",
               (uintmax_t)cb_region_start(&(rr->req.objtable_new_region)),
               (uintmax_t)cb_region_cursor(&(rr->req.objtable_new_region)),
               (uintmax_t)cb_region_end(&(rr->req.objtable_new_region)),
               (uintmax_t)(cb_region_cursor(&(rr->req.objtable_new_region)) - cb_region_start(&(rr->req.objtable_new_region))));

    ret = objtablelayer_traverse((const struct cb **)&(rr->req.orig_cb),
                                 &(rr->req.objtable_c),
                                 copy_objtable_c_not_in_b,
                                 &closure);
    assert(ret == 0);
    KLOX_TRACE("condense objtable 4:  new_root_b: %ju\n", (uintmax_t)rr->resp.objtable_new_b.sm.root_node_offset);
    KLOX_TRACE("done with copy_objtable_c_not_in_b() [s:%ju, c:%ju, e:%ju], used size: %ju\n",
               (uintmax_t)cb_region_start(&(rr->req.objtable_new_region)),
               (uintmax_t)cb_region_cursor(&(rr->req.objtable_new_region)),
               (uintmax_t)cb_region_end(&(rr->req.objtable_new_region)),
               (uintmax_t)(cb_region_cursor(&(rr->req.objtable_new_region)) - cb_region_start(&(rr->req.objtable_new_region))));

    rr->resp.white_list = closure.white_list;
  }

  //Condense tristack
  {
    cb_offset_t   new_bbo              = cb_region_start(&(rr->req.tristack_new_region));
    Value        *new_condensed_values = static_cast<Value*>(cb_at(rr->req.orig_cb, new_bbo));
    Value        *old_c_values         = static_cast<Value*>(cb_at(rr->req.orig_cb, rr->req.tristack_cbo));
    Value        *old_b_values         = static_cast<Value*>(cb_at(rr->req.orig_cb, rr->req.tristack_bbo));
    unsigned int  i                    = rr->req.tristack_cbi;  //(always 0, really)

    assert(i == 0);

    //Copy C section
    while (i < rr->req.tristack_stackDepth && i < rr->req.tristack_bbi) {
      new_condensed_values[i] = old_c_values[i - rr->req.tristack_cbi];
      ++i;
    }

    //Copy B section
    while (i < rr->req.tristack_stackDepth && i < rr->req.tristack_abi) {
      new_condensed_values[i] = old_b_values[i - rr->req.tristack_bbi];
      ++i;
    }

    //Write response
    rr->resp.tristack_new_bbo = new_bbo;
    rr->resp.tristack_new_bbi = rr->req.tristack_cbi;
  }

  // Keep a view of the post-collection tristack setup.
  TriStack new_tristack;
  new_tristack.abo = rr->resp.tristack_new_bbo;
  new_tristack.abi = rr->resp.tristack_new_bbi;
  new_tristack.bbo = 0;
  new_tristack.bbi = 0;
  new_tristack.cbo = 0;
  new_tristack.cbi = 0;
  assert(new_tristack.abi == 0);
  tristack_recache(&new_tristack, thread_cb);

  //Condense triframes
  {
    cb_offset_t   new_bbo              = cb_region_start(&(rr->req.triframes_new_region));
    CallFrame    *new_condensed_frames = static_cast<CallFrame*>(cb_at(rr->req.orig_cb, new_bbo));
    CallFrame    *old_c_frames         = static_cast<CallFrame*>(cb_at(rr->req.orig_cb, rr->req.triframes_cbo));
    CallFrame    *old_b_frames         = static_cast<CallFrame*>(cb_at(rr->req.orig_cb, rr->req.triframes_bbo));
    unsigned int  i                    = rr->req.triframes_cbi;  //(always 0, really)

    assert(i == 0);


    //Create temporary view of what will be the new, consolidated objtable.
    ObjTable consObjtable;
    objtable_init(&consObjtable);
    objtablelayer_assign(&(consObjtable.a), &(rr->resp.objtable_new_b));

    //Copy C section
    while (i < rr->req.triframes_frameCount && i < rr->req.triframes_bbi) {
      CallFrame *src  = &(old_c_frames[i - rr->req.triframes_cbi]);
      CallFrame *dest = &(new_condensed_frames[i]);

      //CallFrames during consolidation are expected to NOT be using the
      //ip_offset member of the union.
      assert(!src->has_ip_offset);

      //KLOX_TRACE("Copying C frame: ");
      //KLOX_TRACE_ONLY(printCallFrame(src));
      //KLOX_TRACE_("\n");
      *dest = *src;

      // Revise the pointers internal to the CallFrame.
      size_t ip_offset = src->ip - src->ip_root;
      dest->functionP = dest->closure.crip_alt(rr->req.orig_cb, &consObjtable).cp()->function.crip_alt(rr->req.orig_cb, &consObjtable).cp();
      dest->constantsValuesP = dest->functionP->chunk.constants.values.crp(rr->req.orig_cb).cp();
      dest->ip_root = dest->functionP->chunk.code.crp(rr->req.orig_cb).cp();
      dest->ip = dest->ip_root + ip_offset;

      ++i;
    }

    //Copy B section
    while (i < rr->req.triframes_frameCount && i < rr->req.triframes_abi) {
      CallFrame *src  = &(old_b_frames[i - rr->req.triframes_bbi]);
      CallFrame *dest = &(new_condensed_frames[i]);

      //CallFrames during consolidation are expected to NOT be using the
      //ip_offset member of the union.
      assert(!src->has_ip_offset);

      //KLOX_TRACE("Copying B frame: ");
      //KLOX_TRACE_ONLY(printCallFrame(src));
      //KLOX_TRACE_("\n");
      *dest = *src;

      // Revise the pointers internal to the CallFrame.
      size_t ip_offset = src->ip - src->ip_root;
      dest->functionP = dest->closure.crip_alt(rr->req.orig_cb, &consObjtable).cp()->function.crip_alt(rr->req.orig_cb, &consObjtable).cp();
      dest->constantsValuesP = dest->functionP->chunk.constants.values.crp(rr->req.orig_cb).cp();
      dest->ip_root = dest->functionP->chunk.code.crp(rr->req.orig_cb).cp();
      dest->ip = dest->ip_root + ip_offset;

      ++i;
    }

    //Write response
    rr->resp.triframes_new_bbo = new_bbo;
    rr->resp.triframes_new_bbi = rr->req.triframes_cbi;
  }

  // Condense strings
  {
    struct copy_strings_closure closure;

    ret = cb_bst_init(&(rr->req.orig_cb),
                      &(rr->req.strings_new_region),
                      &(rr->resp.strings_new_root_b),
                      &klox_value_deep_comparator,
                      &klox_value_deep_comparator,
                      &klox_value_render,
                      &klox_value_render,
                      &klox_no_external_size,
                      &klox_no_external_size);

    closure.src_cb      = rr->req.orig_cb;
    closure.old_root_b  = rr->req.strings_root_b;
    closure.old_root_c  = rr->req.strings_root_c;
    closure.dest_cb     = rr->req.orig_cb;
    closure.dest_region = &(rr->req.strings_new_region);
    closure.new_root_b  = &(rr->resp.strings_new_root_b);
    DEBUG_ONLY(closure.last_bst_size = cb_bst_size(closure.dest_cb, rr->resp.strings_new_root_b));

    ret = cb_bst_traverse(rr->req.orig_cb,
                          rr->req.strings_root_b,
                          copy_strings_b,
                          &closure);
    KLOX_TRACE("done with copy_strings_b() [s:%ju, c:%ju, e:%ju]\n",
               (uintmax_t)cb_region_start(&(rr->req.strings_new_region)),
               (uintmax_t)cb_region_cursor(&(rr->req.strings_new_region)),
               (uintmax_t)cb_region_end(&(rr->req.strings_new_region)));
    assert(ret == 0);

    ret = cb_bst_traverse(rr->req.orig_cb,
                          rr->req.strings_root_c,
                          copy_strings_c_not_in_b,
                          &closure);
    KLOX_TRACE("done with copy_strings_c_not_in_b() [s:%ju, c:%ju, e:%ju]\n",
               (uintmax_t)cb_region_start(&(rr->req.strings_new_region)),
               (uintmax_t)cb_region_cursor(&(rr->req.strings_new_region)),
               (uintmax_t)cb_region_end(&(rr->req.strings_new_region)));
    assert(ret == 0);
  }

  // Condense globals
  {
    struct copy_globals_closure closure;

    ret = cb_bst_init(&(rr->req.orig_cb),
                      &(rr->req.globals_new_region),
                      &(rr->resp.globals_new_root_b),
                      &klox_value_deep_comparator,
                      &klox_value_deep_comparator,
                      &klox_value_render,
                      &klox_value_render,
                      &klox_no_external_size,
                      &klox_no_external_size);

    closure.src_cb      = rr->req.orig_cb;
    closure.old_root_b  = rr->req.globals_root_b;
    closure.old_root_c  = rr->req.globals_root_c;
    closure.dest_cb     = rr->req.orig_cb;
    closure.dest_region = &(rr->req.globals_new_region);
    closure.new_root_b  = &(rr->resp.globals_new_root_b);
    DEBUG_ONLY(closure.last_bst_size = cb_bst_size(closure.dest_cb, rr->resp.globals_new_root_b));

    ret = cb_bst_traverse(rr->req.orig_cb,
                          rr->req.globals_root_b,
                          copy_globals_b,
                          &closure);
    KLOX_TRACE("done with copy_globals_b() [s:%ju, c:%ju, e:%ju]\n",
               (uintmax_t)cb_region_start(&(rr->req.globals_new_region)),
               (uintmax_t)cb_region_cursor(&(rr->req.globals_new_region)),
               (uintmax_t)cb_region_end(&(rr->req.globals_new_region)));
    assert(ret == 0);

    ret = cb_bst_traverse(rr->req.orig_cb,
                          rr->req.globals_root_c,
                          copy_globals_c_not_in_b,
                          &closure);
    KLOX_TRACE("done with copy_globals_c_not_in_b() [s:%ju, c:%ju, e:%ju]\n",
               (uintmax_t)cb_region_start(&(rr->req.globals_new_region)),
               (uintmax_t)cb_region_cursor(&(rr->req.globals_new_region)),
               (uintmax_t)cb_region_end(&(rr->req.globals_new_region)));
    assert(ret == 0);
  }
  return 0;
}
