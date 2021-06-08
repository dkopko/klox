#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cb_bst.h>
#include <cb_region.h>

#include "cb_integration.h"
#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "vm.h"

#ifdef DEBUG_TRACE_GC
#include <stdio.h>
#include "debug.h"
#endif

#define GC_HEAP_GROW_FACTOR 2

#if !KLOX_SYNC_GC
#if PROVOKE_RESIZE_DURING_GC
static bool resize_during_gc_already_provoked = false;
#endif //PROVOKE_RESIZE_DURING_GC
#endif //!KLOX_SYNC_GC

cb_offset_t last_point_of_gc = 0;

size_t
alloc_size_get(const char *mem) {
  size_t size;
  memcpy(&size, mem - sizeof(size_t), sizeof(size_t));
  return size;
}

static void
alloc_size_set(char *mem, size_t size) {
  memcpy(mem - sizeof(size_t), &size, sizeof(size_t));
}

size_t
alloc_alignment_get(const char *mem) {
  size_t alignment;
  memcpy(&alignment, mem - (2 * sizeof(size_t)), sizeof(size_t));
  return alignment;
}

static void
alloc_alignment_set(char *mem, size_t alignment) {
  memcpy(mem - (2 * sizeof(size_t)), &alignment, sizeof(size_t));
}

bool
alloc_is_object_get(const char *mem) {
  bool is_object;
  memcpy(&is_object, mem - (2 * sizeof(size_t) + sizeof(bool)), sizeof(bool));
  return is_object;
}

static void
alloc_is_object_set(char *mem, bool is_object) {
  memcpy(mem - (2 * sizeof(size_t) + sizeof(bool)) , &is_object, sizeof(bool));
}

static inline void
clobber_mem(void *p, size_t len) {
#ifdef DEBUG_CLOBBER
    memset(p, '!', len);
#endif
}

cb_offset_t reallocate_within(struct cb **cb, struct cb_region *region, cb_offset_t previous, size_t oldSize, size_t newSize, size_t alignment, bool isObject, bool suppress_gc) {
  vm.bytesAllocated += newSize - oldSize;

  bool needs_gc = false;
  if (!suppress_gc) {
    needs_gc =
#ifdef DEBUG_STRESS_GC
      exec_phase != EXEC_PHASE_COMPILE ||   //NOTE: must exclude this phase because of the fact we PIN_SCOPE across the whole phase, so cannot afford to do many collections.
#endif
      (newSize > oldSize && vm.bytesAllocated > vm.nextGC);
  }

  if (needs_gc) {
    if (!gc_request_is_outstanding)
      collectGarbage();
  }

#ifndef NDEBUG
  if (previous != CB_NULL) {
    //Check that old values are as expected, given that this function
    //historically has expected to be given the old size and alignments
    //cannot change over reallocate()s.
    char *mem = (char *)cb_at(*cb, previous);
    assert(alloc_size_get(mem) == oldSize);
    assert(alloc_alignment_get(mem) == alignment);
    assert(alloc_is_object_get(mem) == isObject);
    (void)mem;
  }
#endif

  if (newSize == 0) {
    if (previous != CB_NULL)
      clobber_mem(cb_at(*cb, previous), oldSize);
    return CB_NULL;
  } else if (newSize < oldSize) {
    if (previous != CB_NULL)
      clobber_mem(((char *)cb_at(*cb, previous)) + newSize, oldSize - newSize);
    return previous;
  } else {
    size_t header_size = sizeof(size_t)   /* size field */
                         + sizeof(size_t) /* alignment field */
                         + sizeof(bool);  /* isObject field */
    size_t needed_contiguous_size = header_size + (alignment - 1) + newSize;
    cb_offset_t new_offset;
    int ret;

    (void)ret;

    ret = cb_region_memalign(cb,
                             region,
                             &new_offset,
                             alignment,
                             needed_contiguous_size);
    assert(ret == CB_SUCCESS);

    new_offset = cb_offset_aligned_gte(new_offset + header_size, alignment);

    char *mem = (char *)cb_at(*cb, new_offset);
    alloc_size_set(mem, newSize);
    alloc_alignment_set(mem, alignment);
    alloc_is_object_set(mem, isObject);

    //Q: Should we keep the ObjID the same over reallocation?
    //A: No, changing it adheres to the earlier API which expects a shift of
    // offset (or earlier API than that, pointer).  Although it may work to
    // leave the ObjID the same, this may gloss over errors elsewhere, so we
    // force it to change for the sake of provoking any such errors.
    if (previous != CB_NULL) {
      char *prevMem = (char *)cb_at(*cb, previous);
      memcpy(mem, prevMem, oldSize);
      clobber_mem(prevMem, oldSize);
    }

    return new_offset;
  }
}

cb_offset_t reallocate(cb_offset_t previous, size_t oldSize, size_t newSize, size_t alignment, bool isObject, bool suppress_gc) {
  return reallocate_within(&thread_cb, &thread_region, previous, oldSize, newSize, alignment, isObject, suppress_gc);
}

bool objectIsDark(const OID<Obj> objectOID) {
  cb_term key_term;

  cb_term_set_u64(&key_term, objectOID.id().id);

  return cb_bst_contains_key(gc_thread_cb,
                             gc_thread_darkset_bst,
                             &key_term);
}

static void objectSetDark(OID<Obj> objectOID) {
  cb_term key_term;
  cb_term value_term;
  int ret;

  cb_term_set_u64(&key_term, objectOID.id().id);
  cb_term_set_u64(&value_term, objectOID.id().id);

  ret = cb_bst_insert(&gc_thread_cb,
                      &gc_thread_region,
                      &gc_thread_darkset_bst,
                      0,
                      &key_term,
                      &value_term);
  assert(ret == 0);
  (void)ret;

  //NOTE: We cannot validate growth vs. estimated max growth in this function
  // because we are not allocating into a static region whose size is known
  // ahead of time.  As such, regions may be being created dynamically to
  // fulfill the insertion, and region cursor positions can therefore differ
  // by large amounts unrelated to actual bst insertion allocations.
}

void clearDarkObjectSet(void) {
  gc_thread_darkset_bst = CB_BST_SENTINEL;
}

void grayObject(const OID<Obj> objectOID) {
  if (objectOID.is_nil()) return;

  // Don't get caught in cycle.
  if (objectIsDark(objectOID)) return;

#ifdef DEBUG_TRACE_GC
  KLOX_TRACE("id: #%ju, obj: ", (uintmax_t)objectOID.id().id);
  KLOX_TRACE_ONLY(printValue(OBJ_VAL(objectOID.id()), false));
  KLOX_TRACE_("\n");
#endif

  objectSetDark(objectOID);

  if (gc.grayCapacity < gc.grayCount + 1) {
    int oldGrayCapacity = gc.grayCapacity;
    cb_offset_t oldGrayStackOffset = gc.grayStack.co();
    gc.grayCapacity = GROW_CAPACITY(gc.grayCapacity);

    gc.grayStack = reallocate_within(&gc_thread_cb,
                                     &gc_thread_region,
                                     oldGrayStackOffset,
                                     sizeof(OID<Obj>) * oldGrayCapacity,
                                     sizeof(OID<Obj>) * gc.grayCapacity,
                                     cb_alignof(OID<Obj>),
                                     false,
                                     true);

#ifdef DEBUG_TRACE_GC
    KLOX_TRACE("@%ju OID<Obj>[%zd] array allocated (%zd bytes) (resized from @%ju OID<Obj>[%zd] array (%zd bytes))\n",
               (uintmax_t)gc.grayStack.co(),
               (size_t)gc.grayCapacity,
               sizeof(OID<Obj*>) * gc.grayCapacity,
               (uintmax_t)oldGrayStackOffset,
               (size_t)oldGrayCapacity,
               sizeof(OID<Obj*>) * oldGrayCapacity);
#endif

  }

  gc.grayStack.mrp(gc_thread_cb).mp()[gc.grayCount++] = objectOID;
}

void grayValue(Value value) {
  if (!IS_OBJ(value)) return;
  grayObject(AS_OBJ_ID(value));
}

static bool isWhiteObject(OID<Obj> objectOID) {
  if (objectOID.is_nil()) return true;
  if (objectIsDark(objectOID)) return false;
  return true;
}

bool isWhite(Value value) {
  if (!IS_OBJ(value)) return true;
  return isWhiteObject(AS_OBJ_ID(value));
}

static int
bstTraversalGray(const struct cb_term *key_term,
                 const struct cb_term *value_term,
                 void                 *closure)
{
  Value keyValue;
  Value valueValue;

  keyValue = numToValue(cb_term_get_dbl(key_term));
  valueValue = numToValue(cb_term_get_dbl(value_term));

  grayValue(keyValue);
  grayValue(valueValue);

  return 0;
}

static void grayBst(cb_offset_t bst) {
  int ret;

  (void)ret;

  ret = cb_bst_traverse(thread_cb,
                        bst,
                        &bstTraversalGray,
                        NULL);
  assert(ret == 0);
}

static int
structmapTraversalGray(uint64_t k, uint64_t v, void *closure)
{
  ObjID ko = { .id = k };
  Value keyValue = OBJ_VAL(ko);
  Value valueValue = { .val = v };

  grayValue(keyValue);
  grayValue(valueValue);

  return 0;
}

static void grayMethodsStructmap(const MethodsSM *sm) {
  int ret;

  (void)ret;

  ret = sm->traverse((const struct cb **)&thread_cb,
                     &structmapTraversalGray,
                     NULL);
  assert(ret == 0);
}

static void grayFieldsStructmap(const FieldsSM *sm) {
  int ret;

  (void)ret;

  ret = sm->traverse((const struct cb **)&thread_cb,
                     &structmapTraversalGray,
                     NULL);
  assert(ret == 0);
}

void grayObjectLeaves(const OID<Obj> objectOID) {
  const Obj *object;
  bool found_in_b;

  //There should never be any A region contents during this call.
  assert(!objectOID.clipA().cp());

  //Find the Obj whose leaves are to be darkened in either the B or C region.
  object = objectOID.clipB().cp();  //NOTE: we're in GC thread and therefore this is not RCBP (as GC must not cause resize).
  found_in_b = !!object;
  if (!found_in_b) object = objectOID.clipC().cp();

#ifdef DEBUG_TRACE_GC
  KLOX_TRACE("id: #%ju, obj: ", (uintmax_t)objectOID.id().id);
  KLOX_TRACE_ONLY(printValue(OBJ_VAL(objectOID.id()), false));
  KLOX_TRACE_("\n");
#endif

  switch (object->type) {
    case OBJ_BOUND_METHOD: {
      const ObjBoundMethod* bound = (const ObjBoundMethod*)object;
      grayValue(bound->receiver);
      grayObject(bound->method.id());
      break;
    }

    case OBJ_CLASS: {
      const ObjClass* klass = (const ObjClass*)object;
      grayObject(klass->name.id());
      grayObject(klass->superclass.id());
      grayMethodsStructmap(&(klass->methods_sm));

      //NOTE: Classes are represented by ObjClass layers.  The garbage
      // collector only deals with regions B and C.  If retrieval of the
      // objectOID has given us a B region ObjClass layer, then also gray any
      // methods of any backing C region ObjClass layer.
      if (found_in_b) {
        const ObjClass* klass_C = (const ObjClass*)objectOID.clipC().cp();
        if (klass_C) {
          KLOX_TRACE("found backing class for #%ju\n", objectOID.id().id);
          grayMethodsStructmap(&(klass_C->methods_sm));
        }
      }
      break;
    }

    case OBJ_CLOSURE: {
      const ObjClosure* closure = (const ObjClosure*)object;
      grayObject(closure->function.id());
      for (int i = 0; i < closure->upvalueCount; i++) {
        grayObject(closure->upvalues.clp().cp()[i].id());
      }
      break;
    }

    case OBJ_FUNCTION: {
      const ObjFunction* function = (const ObjFunction*)object;
      grayObject(function->name.id());
      for (int i = 0; i < function->chunk.constants.count; i++) {
        grayValue(function->chunk.constants.values.clp().cp()[i]);
      }
      break;
    }

    case OBJ_INSTANCE: {
      const ObjInstance* instance = (const ObjInstance*)object;
      grayObject(instance->klass.id());
      grayFieldsStructmap(&(instance->fields_sm));

      //NOTE: Instances are represented by ObjInstance layers.  The garbage
      // collector only deals with regions B and C.  If retrieval of the
      // objectOID has given us a B region ObjInstance layer, then also gray any
      // fields of any backing C region ObjInstance layer.
      if (found_in_b) {
        const ObjInstance* instance_C = (const ObjInstance*)objectOID.clipC().cp();
        if (instance_C) {
          KLOX_TRACE("found backing instance for #%ju\n", objectOID.id().id);
          grayFieldsStructmap(&(instance_C->fields_sm));
        }
      }
      break;
    }

    case OBJ_UPVALUE:
      grayValue(((const ObjUpvalue*)object)->closed);
      break;

    case OBJ_NATIVE:
    case OBJ_STRING:
      // No references.
      break;
  }
}

static void freeObject(OID<Obj> object) {
#ifdef DEBUG_TRACE_GC
  KLOX_TRACE("id: #%ju, obj: ", (uintmax_t)object.id().id);
  KLOX_TRACE_ONLY(printValue(OBJ_VAL(object.id()), false));
  KLOX_TRACE_("\n");
#endif

  //NOTE: We can no longer clobber old memory contents due to the fact that
  // older maps (e.g. regions B and C of objtable) may still be working with
  // the objects.  Instead, freeObject() now just means to nullify the referred
  // #ID, so that it can no longer be dereferenced via the runtime of the
  // executing program and will eventually have no @offset associated with it in
  // any region of the objtable map.
  objtable_invalidate(&thread_objtable, object.id());
}

//NOTE: This function may be called from both the main execution thread, or from
// the GC thread.  In the latter case, the RCBP's are not a problem because:
// 1) The linked list on which they will be held will be thread-local to the
//    GC thread (and so will not race with nor interfere with the list on the
//    main executor thread).
// 2) The GC must itself never cause a resize, so the list will never be used.
cb_offset_t deriveMutableObjectLayer(struct cb **cb, struct cb_region *region, ObjID id, cb_offset_t object_offset) {
  PIN_SCOPE;
  CBO<Obj> srcCBO = object_offset; // Avoids ObjTable lookup for the switch.  (Be sure not to use after a reallocate_within(), as a provoked GC would invalidate!)
  OID<Obj> srcOID = id;
  CBO<Obj> destCBO;
  bool suppress_gc = (on_main_thread == false || exec_phase != EXEC_PHASE_INTERPRET);  //prevent cycles

  KLOX_TRACE("src: #%ju@%ju, obj: ", (uintmax_t)id.id, object_offset);
  KLOX_TRACE_ONLY(printObject(id, object_offset, srcOID.crip(*cb).cp(), false));
  KLOX_TRACE_("\n");

  switch (srcCBO.crp(*cb).cp()->type) {
    case OBJ_BOUND_METHOD: {
      destCBO = reallocate_within(cb, region, CB_NULL, 0, sizeof(ObjBoundMethod), cb_alignof(ObjBoundMethod), true, suppress_gc);
      const ObjBoundMethod *src  = (const ObjBoundMethod *)srcOID.crip(*cb).cp();  //cb-resize-safe (no allocations in lifetime)
      ObjBoundMethod       *dest = (ObjBoundMethod *)destCBO.mrp(*cb).mp();  //cb-resize-safe (no allocations in lifetime)

      dest->obj      = src->obj;
      dest->receiver = src->receiver;
      dest->method   = src->method;

      break;
    }

    case OBJ_CLASS: {
      destCBO = reallocate_within(cb, region, CB_NULL, 0, sizeof(ObjClass), cb_alignof(ObjClass), true, suppress_gc);
      const ObjClass *src  = (const ObjClass *)srcOID.crip(*cb).cp();  //cb-resize-safe (no allocations in lifetime)
      ObjClass       *dest = (ObjClass *)destCBO.mrp(*cb).mp();  //cb-resize-safe (no allocations in lifetime)

      dest->obj         = src->obj;
      dest->name        = src->name;
      dest->superclass  = src->superclass;
      //NOTE: We expect lookup of methods to first check this new, mutable,
      //  A-region ObjClass, before looking at older versions in B and C.
      methods_layer_init(cb, region, &(dest->methods_sm));

      break;
    }

    case OBJ_CLOSURE: {
      destCBO = reallocate_within(cb, region, CB_NULL, 0, sizeof(ObjClosure), cb_alignof(ObjClosure), true, suppress_gc);
      RCBP<const ObjClosure> srcR = srcOID.crip(*cb);
      CBO<OID<ObjUpvalue> > newDestUpvalues = GROW_ARRAY_NOGC_WITHIN(cb, region, CB_NULL, OID<ObjUpvalue>, 0, srcR.cp()->upvalueCount);
      const ObjClosure *src = srcR.cp();  //cb-resize-safe (no allocations in lifetime)
      ObjClosure *dest = (ObjClosure *)destCBO.mrp(*cb).mp();  //cb-resize-safe (no allocations in lifetime)

      dest->obj      = src->obj;
      dest->function = src->function;
      dest->upvalues = newDestUpvalues;
      {
        const OID<ObjUpvalue> *srcUpvalues = src->upvalues.crp(*cb).cp();  //cb-resize-safe (no allocations in lifetime)
        OID<ObjUpvalue> *destUpvalues = dest->upvalues.mrp(*cb).mp();  //cb-resize-safe (no allocations in lifetime)

        for (unsigned int i = 0, e = src->upvalueCount; i < e; ++i) {
          //KLOX_TRACE("copying upvalue (srcUpvalues:%p, srcUpvalues[%d].id(): %ju, srcUpvalues[%d].clip():%p\n",
          //    srcUpvalues, i, (uintmax_t)srcUpvalues[i].id().id, i, srcUpvalues[i].clip().cp());
          //KLOX_TRACE_ONLY(printValue(srcUpvalues[i].clip()->closed));
          //KLOX_TRACE_("\n");
          destUpvalues[i] = srcUpvalues[i];
        }
      }
      dest->upvalueCount = src->upvalueCount;

      break;
    }

    case OBJ_FUNCTION: {
      destCBO = reallocate_within(cb, region, CB_NULL, 0, sizeof(ObjFunction), cb_alignof(ObjFunction), true, suppress_gc);
      RCBP<const ObjFunction> srcR = srcOID.crip(*cb);
      CBO<uint8_t> newCode   = GROW_ARRAY_NOGC_WITHIN(cb, region, CB_NULL, uint8_t, 0, srcR.cp()->chunk.capacity);
      CBO<int>     newLines  = GROW_ARRAY_NOGC_WITHIN(cb, region, CB_NULL, int, 0, srcR.cp()->chunk.capacity);
      CBO<Value>   newValues = GROW_ARRAY_NOGC_WITHIN(cb, region, CB_NULL, Value, 0, srcR.cp()->chunk.constants.capacity);
      const ObjFunction *src = srcR.cp();  //cb-resize-safe (no allocations in lifetime)
      ObjFunction *dest = (ObjFunction *)destCBO.mrp(*cb).mp();  //cb-resize-safe (no allocations in lifetime)

      dest->obj          = src->obj;
      dest->arity        = src->arity;
      dest->upvalueCount = src->upvalueCount;
      dest->chunk.count    = src->chunk.count;
      dest->chunk.capacity = src->chunk.capacity;
      dest->chunk.code = newCode;
      memcpy(dest->chunk.code.mrp(*cb).mp(), src->chunk.code.crp(*cb).cp(), src->chunk.capacity * sizeof(uint8_t));
      dest->chunk.lines = newLines;
      memcpy(dest->chunk.lines.mrp(*cb).mp(), src->chunk.lines.crp(*cb).cp(), src->chunk.capacity * sizeof(int));
      dest->chunk.constants.capacity = src->chunk.constants.capacity;
      dest->chunk.constants.count    = src->chunk.constants.count;
      dest->chunk.constants.values   = newValues;
      memcpy(dest->chunk.constants.values.mrp(*cb).mp(), src->chunk.constants.values.crp(*cb).cp(), src->chunk.constants.capacity * sizeof(Value));
      dest->name         = src->name;

      break;
    }

    case OBJ_INSTANCE: {
      destCBO = reallocate_within(cb, region, CB_NULL, 0, sizeof(ObjInstance), cb_alignof(ObjInstance), true, suppress_gc);
      const ObjInstance *src  = (const ObjInstance *)srcOID.crip(*cb).cp();  //cb-resize-safe (no allocations in lifetime)
      ObjInstance       *dest = (ObjInstance *)destCBO.mrp(*cb).mp();  //cb-resize-safe (no allocations in lifetime)

      dest->obj        = src->obj;
      dest->klass      = src->klass;
      //NOTE: We expect lookup of fields to first check this new, mutable,
      //  A-region ObjClass, before looking at older versions in B and C.
      fields_layer_init(cb, region, &(dest->fields_sm));

      break;
    }

    case OBJ_NATIVE: {
      destCBO = reallocate_within(cb, region, CB_NULL, 0, sizeof(ObjNative), cb_alignof(ObjNative), true, suppress_gc);
      const ObjNative *src  = (const ObjNative *)srcOID.crip(*cb).cp();  //cb-resize-safe (no allocations in lifetime)
      ObjNative       *dest = (ObjNative *)destCBO.mrp(*cb).mp();  //cb-resize-safe (no allocations in lifetime)

      dest->obj      = src->obj;
      dest->function = src->function;

      break;
    }

    case OBJ_STRING: {
      destCBO = reallocate_within(cb, region, CB_NULL, 0, sizeof(ObjString), cb_alignof(ObjString), true, suppress_gc);
      RCBP<const ObjString> srcR  = srcOID.crip(*cb);
      CBO<char> newChars = GROW_ARRAY_NOGC_WITHIN(cb, region, CB_NULL, char, 0, srcR.cp()->length + 1);
      const ObjString *src  = srcR.cp();  //cb-resize-safe (no allocations in lifetime)
      ObjString       *dest = (ObjString *)destCBO.mrp(*cb).mp();  //cb-resize-safe (no allocations in lifetime)

      dest->obj    = src->obj;
      dest->length = src->length;
      dest->chars  = newChars;
      char *destChars = dest->chars.mrp(*cb).mp();
      memcpy(destChars, src->chars.crp(*cb).cp(), src->length * sizeof(char));
      destChars[src->length] = '\0';
      dest->hash   = src->hash;

      break;
    }

    case OBJ_UPVALUE: {
      destCBO = reallocate_within(cb, region, CB_NULL, 0, sizeof(ObjUpvalue), cb_alignof(ObjUpvalue), true, suppress_gc);
      const ObjUpvalue *src  = (const ObjUpvalue *)srcOID.crip(*cb).cp();  //cb-resize-safe (no allocations in lifetime)
      ObjUpvalue       *dest = (ObjUpvalue *)destCBO.mrp(*cb).mp();  //cb-resize-safe (no allocations in lifetime)

      dest->obj             = src->obj;
      dest->valueStackIndex = src->valueStackIndex;
      dest->closed          = src->closed;
      dest->next            = src->next;

      break;
    }

    default:
      printf("badobj'%c'#%ju", srcCBO.crp(*cb).cp()->type, (uintmax_t)id.id);
      abort();
  }

  return destCBO.mo();
}

struct copy_entry_closure
{
  struct cb        **dest_cb;
  struct cb_region  *dest_region;
  cb_offset_t       *dest_bst;
  DEBUG_ONLY(size_t  last_bst_size);
};

static int
copy_entry_to_bst(const struct cb_term *key_term,
                  const struct cb_term *value_term,
                  void                 *closure)
{
  struct copy_entry_closure *cl = (struct copy_entry_closure *)closure;
  int ret;

  (void)ret;

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));

  ret = cb_bst_insert(cl->dest_cb,
                      cl->dest_region,
                      cl->dest_bst,
                      cb_region_start(cl->dest_region),  //NOTE: full contents are mutable
                      key_term,
                      value_term);

  assert(ret == 0);
  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));
  DEBUG_ONLY(size_t      bst_size = cb_bst_size(*(cl->dest_cb), *cl->dest_bst));

  // Actual bytes used must be <= BST's self-considered growth.
  assert(c1 - c0 <= bst_size - cl->last_bst_size);
  DEBUG_ONLY(cl->last_bst_size = bst_size);

  return 0;
}

struct copy_MethodsSM_entry_closure
{
  struct cb        **dest_cb;
  struct cb_region  *dest_region;
  MethodsSM         *dest_sm;
  DEBUG_ONLY(size_t  last_sm_size);
};

static int
copy_MethodsSM_entry(uint64_t k, uint64_t v, void *closure)
{
  struct copy_MethodsSM_entry_closure *cl = (struct copy_MethodsSM_entry_closure *)closure;
  int ret;

  (void)ret;

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));

  ret = cl->dest_sm->insert(cl->dest_cb,
                            cl->dest_region,
                            k,
                            v);
  assert(ret == 0);

  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));
  DEBUG_ONLY(size_t      sm_size = cl->dest_sm->size());

  // Actual bytes used must be <= structmap's self-considered growth.
  assert(c1 - c0 <= sm_size - cl->last_sm_size);
  DEBUG_ONLY(cl->last_sm_size = sm_size);

  return 0;
}

struct copy_FieldsSM_entry_closure
{
  struct cb        **dest_cb;
  struct cb_region  *dest_region;
  FieldsSM          *dest_sm;
  DEBUG_ONLY(size_t  last_sm_size);
};

static int
copy_FieldsSM_entry(uint64_t k, uint64_t v, void *closure)
{
  struct copy_FieldsSM_entry_closure *cl = (struct copy_FieldsSM_entry_closure *)closure;
  int ret;

  (void)ret;

  DEBUG_ONLY(cb_offset_t c0 = cb_region_cursor(cl->dest_region));

  ret = cl->dest_sm->insert(cl->dest_cb,
                            cl->dest_region,
                            k,
                            v);
  assert(ret == 0);
  DEBUG_ONLY(cb_offset_t c1 = cb_region_cursor(cl->dest_region));
  DEBUG_ONLY(size_t      sm_size = cl->dest_sm->size());

  // Actual bytes used must be <= structmap's self-considered growth.
  assert(c1 - c0 <= sm_size - cl->last_sm_size);
  DEBUG_ONLY(cl->last_sm_size = sm_size);

  return 0;
}

cb_offset_t cloneObject(struct cb **cb, struct cb_region *region, ObjID id, cb_offset_t object_offset) {
  assert(gc_phase == GC_PHASE_CONSOLIDATE);

  CBO<Obj> srcCBO = object_offset;
  CBO<Obj> cloneCBO = deriveMutableObjectLayer(cb, region, id, object_offset);
  int ret;

  (void)ret;

  KLOX_TRACE("#%ju@%ju cloneObject() ", (uintmax_t)id.id, object_offset);
  KLOX_TRACE_ONLY(printObject(id, object_offset, srcCBO.clp().cp(), false));
  KLOX_TRACE_(" : NEW OFFSET = %ju\n", (uintmax_t)cloneCBO.mo());

  //NOTE: ObjClasses and ObjInstances come out of deriveMutableObjectLayer()
  // without contents in their respective methods_sm/fields_sm, so the
  // contents must be copied in separately here.
  switch (srcCBO.clp().cp()->type) {
    case OBJ_CLASS: {
      ObjClass *srcClass = (ObjClass *)srcCBO.crp(*cb).cp();  //cb-resize-safe (GC preallocated space)
      ObjClass *destClass = (ObjClass *)cloneCBO.crp(*cb).cp();  //cb-resize-safe (GC prealloated space)
      struct copy_MethodsSM_entry_closure cl = {
        .dest_cb = cb,
        .dest_region = region,
        .dest_sm = &(destClass->methods_sm),
        DEBUG_ONLY(cl.last_sm_size = destClass->methods_sm.size())
      };

      ret = srcClass->methods_sm.traverse((const struct cb **)cb,
                                          copy_MethodsSM_entry,
                                          &cl);
      assert(ret == 0);
    }
    break;

    case OBJ_INSTANCE: {
      ObjInstance *srcInstance = (ObjInstance *)srcCBO.crp(*cb).cp();  //cb-resize-safe (GC preallocated space)
      ObjInstance *destInstance = (ObjInstance *)cloneCBO.crp(*cb).cp();  //cb-resize-safe (GC preallocated space)
      struct copy_FieldsSM_entry_closure cl = {
        .dest_cb = cb,
        .dest_region = region,
        .dest_sm = &(destInstance->fields_sm),
        DEBUG_ONLY(cl.last_sm_size = destInstance->fields_sm.size())
      };

      ret = srcInstance->fields_sm.traverse((const struct cb **)cb,
                                            copy_FieldsSM_entry,
                                            &cl);
      assert(ret == 0);
    }
    break;

    default:
      break;
  }

  return cloneCBO.mo();
}

void freezeARegions(cb_offset_t new_lower_bound) {
  int ret;

  (void)ret;

  assert(on_main_thread);

  // Objtable
  objtable_freeze(&thread_objtable, &thread_cb, &thread_region);

  // Tristack
  assert(vm.tristack.cbo == CB_NULL);
  assert(vm.tristack.cbi == 0);
  vm.tristack.cbo = vm.tristack.bbo;
  vm.tristack.cbi = vm.tristack.bbi;
  vm.tristack.bbo = vm.tristack.abo;
  vm.tristack.bbi = vm.tristack.abi;
  ret = cb_region_memalign(&thread_cb,
                           &thread_region,
                           &(vm.tristack.abo),
                           cb_alignof(Value),
                           sizeof(Value) * STACK_MAX);
  assert(ret == CB_SUCCESS);
  vm.tristack.abi = vm.tristack.stackDepth;
  assert(vm.tristack.abo >= new_lower_bound);
  tristack_recache(&(vm.tristack), thread_cb);

  // Triframes
  assert(vm.triframes.cbo == CB_NULL);
  assert(vm.triframes.cbi == 0);
  vm.triframes.cbo = vm.triframes.bbo;
  vm.triframes.cbi = vm.triframes.bbi;
  vm.triframes.bbo = vm.triframes.abo;
  vm.triframes.bbi = vm.triframes.abi;
  ret = cb_region_memalign(&thread_cb,
                           &thread_region,
                           &(vm.triframes.abo),
                           cb_alignof(CallFrame),
                           sizeof(CallFrame) * FRAMES_MAX);
  assert(ret == CB_SUCCESS);
  vm.triframes.abi = vm.triframes.frameCount;
  assert(vm.triframes.abo >= new_lower_bound);
  triframes_recache(&(vm.triframes), thread_cb);
  vm.currentFrame = vm.triframes.currentFrame;

  // Strings
  assert(cb_bst_num_entries(thread_cb, vm.strings.root_c) == 0);
  vm.strings.root_c = vm.strings.root_b;
  vm.strings.root_b = vm.strings.root_a;
  ret = cb_bst_init(&thread_cb,
                    &thread_region,
                    &(vm.strings.root_a),
                    &klox_value_deep_comparator,
                    &klox_value_deep_comparator,
                    &klox_value_render,
                    &klox_value_render,
                    &klox_no_external_size,
                    &klox_no_external_size);
  assert(ret == 0);
  assert(vm.strings.root_a >= new_lower_bound);

  // Globals
  assert(cb_bst_num_entries(thread_cb, vm.globals.root_c) == 0);
  vm.globals.root_c = vm.globals.root_b;
  vm.globals.root_b = vm.globals.root_a;
  ret = cb_bst_init(&thread_cb,
                    &thread_region,
                    &(vm.globals.root_a),
                    &klox_value_deep_comparator,
                    &klox_value_deep_comparator,
                    &klox_value_render,
                    &klox_value_render,
                    &klox_no_external_size,
                    &klox_no_external_size);
  assert(ret == 0);
  assert(vm.globals.root_a >= new_lower_bound);
}


struct print_objtable_closure
{
  const char  *desc;
};

static int
printObjtableTraversal(uint64_t  key,
                       uint64_t  val,
                       void     *closure)
{
  struct print_objtable_closure *poc = (struct print_objtable_closure *)closure;
  ObjID objID = { .id = key };
  cb_offset_t offset = (cb_offset_t)val;

  (void)poc, (void)objID, (void)offset;

  KLOX_TRACE("%s #%ju -> @%ju\n",
             poc->desc,
             (uintmax_t)objID.id,
             (uintmax_t)offset);

  return 0;
}

void printStateOfWorld(const char *desc) {
  int ret;

  (void)ret;

  KLOX_TRACE("===== BEGIN STATE OF WORLD %s (gc: %u) =====\n", desc, gc_integration_epoch);

  KLOX_TRACE("----- begin objtable (a:%ju, asz:%zu, b:%ju, bsz:%zu, c:%ju, csz:%zu)-----\n",
             thread_objtable.a.sm->root_node_offset,
             objtablelayer_size(&(thread_objtable.a)),
             thread_objtable.b.sm->root_node_offset,
             objtablelayer_size(&(thread_objtable.b)),
             thread_objtable.c.sm->root_node_offset,
             objtablelayer_size(&(thread_objtable.c)));

  struct print_objtable_closure poc;

  poc.desc = "A";
  ret = objtablelayer_traverse((const struct cb **)&thread_cb,
                               &(thread_objtable.a),
                               &printObjtableTraversal,
                               &poc);
  assert(ret == 0);

  poc.desc = "B";
  ret = objtablelayer_traverse((const struct cb **)&thread_cb,
                               &(thread_objtable.b),
                               &printObjtableTraversal,
                               &poc);
  assert(ret == 0);

  poc.desc = "C";
  ret = objtablelayer_traverse((const struct cb **)&thread_cb,
                               &(thread_objtable.c),
                               &printObjtableTraversal,
                               &poc);
  assert(ret == 0);

  KLOX_TRACE("----- end objtable -----\n");

  KLOX_TRACE("----- begin vm.strings -----\n");
  KLOX_TRACE_ONLY(printTable(&vm.strings, "vm.strings"));
  KLOX_TRACE("----- end vm.strings -----\n");

  KLOX_TRACE("----- begin vm.globals -----\n");
  KLOX_TRACE_ONLY(printTable(&vm.globals, "vm.globals"));
  KLOX_TRACE("----- end vm.globals -----\n");

  KLOX_TRACE("----- begin vm.tristack (abo: %ju, abi: %u, bbo: %ju, bbi: %u, cbo: %ju, cbi: %u-----\n",
      (uintmax_t)vm.tristack.abo, vm.tristack.abi,
      (uintmax_t)vm.tristack.bbo, vm.tristack.bbi,
      (uintmax_t)vm.tristack.cbo, vm.tristack.cbi);
  KLOX_TRACE_ONLY(tristack_print(&(vm.tristack)));
  KLOX_TRACE("----- end vm.tristack -----\n");

  KLOX_TRACE("----- begin vm.triframes (abo: %ju, abi: %u, bbo: %ju, bbi: %u, cbo: %ju, cbi: %u-----\n",
      (uintmax_t)vm.triframes.abo, vm.triframes.abi,
      (uintmax_t)vm.triframes.bbo, vm.triframes.bbi,
      (uintmax_t)vm.triframes.cbo, vm.triframes.cbi);
  KLOX_TRACE_ONLY(triframes_print(&(vm.triframes)));
  KLOX_TRACE("----- end vm.triframes -----\n");

  KLOX_TRACE("----- begin vm.openUpvalues -----\n");
  for (OID<ObjUpvalue> upvalue = vm.openUpvalues;
       !upvalue.is_nil();
       upvalue = upvalue.clip().cp()->next) {
    KLOX_TRACE("");
    KLOX_TRACE_ONLY(printObject(upvalue.id(), upvalue.co(), (const Obj *)upvalue.clip().cp(), false));
    KLOX_TRACE_("\n");
  }
  KLOX_TRACE("----- end vm.openUpvalues -----\n");

  KLOX_TRACE("===== END STATE OF WORLD %s (gc: %u) =====\n", desc, gc_integration_epoch);
}

void
mprotect_all_except_gc_region(struct cb *thread_cb, cb_offset_t gc_start_offset, cb_offset_t gc_end_offset)
{
  long pagesize    = sysconf(_SC_PAGESIZE);
  char *ring_start = (char*)cb_ring_start(thread_cb);
  char *ring_end   = (char*)cb_ring_end(thread_cb);
  char *gc_start   = (char*)cb_at(thread_cb, gc_start_offset);
  char *gc_end     = (char*)cb_at(thread_cb, gc_end_offset);
  int ret;

  (void)ret;

  assert(pagesize > 0 && is_power_of_2(pagesize));
  assert((uintptr_t)ring_start % pagesize == 0);
  assert((uintptr_t)ring_end % pagesize == 0);
  assert((uintptr_t)gc_start % pagesize == 0);
  assert((uintptr_t)gc_end % pagesize == 0);

  KLOX_TRACE("DANDEBUG mprotect()ing ring with gc range: [%ju, %ju)\n",
             (uintmax_t)gc_start_offset,
             (uintmax_t)gc_end_offset);


  //Protect the header.
  for (char *m = (char*)thread_cb; m < ring_start; m += pagesize) {
    ret = mprotect(thread_cb, pagesize, PROT_READ | PROT_WRITE);
    assert(ret == 0);
  }

  if (gc_start < gc_end) {
    //Protect the ring (except for the target region the GC thread would write to).
    for (char *m = ring_start; m < ring_end; m += pagesize) {
      ret = mprotect(m, pagesize, (m >= gc_start && m < gc_end) ? (PROT_READ | PROT_WRITE) : PROT_READ);
      assert(ret == 0);
    }

    //Protect the loop.
    for (char *m = ring_end, *e = ring_end + cb_loop_size(thread_cb); m < e; m += pagesize) {
      ret = mprotect(m, pagesize, PROT_READ);
      assert(ret == 0);
    }
  } else {
    //Allow writes to the latter portion of the GC destination range (which exists at the start of the ring).
    for (char *m = ring_start; m < gc_end ; m += pagesize) {
      ret = mprotect(m, pagesize, (PROT_READ | PROT_WRITE));
      assert(ret == 0);
    }

    //Disallow writes to the area between the end of the gc region and the start of it.
    for (char *m = gc_end; m < gc_start; m += pagesize) {
      ret = mprotect(m, pagesize, PROT_READ);
      assert(ret == 0);
    }

    //Allow writes from the start of the GC destination region and through the loop.
    //(The loop must be fully enlcosed by [gc_start, gc_end) because if it were not then
    // gc_end would have resolved within it and the addresses would have had the relation
    // gc_start < gc_end .)
    for (char *m = gc_start, *e = ring_end + cb_loop_size(thread_cb); m < e; m += pagesize) {
      ret = mprotect(m, pagesize, (PROT_READ | PROT_WRITE));
      assert(ret == 0);
    }
  }
}

void
unmprotect_all(struct cb *thread_cb)
{
  long  pagesize   = sysconf(_SC_PAGESIZE);
  char *ring_start = (char*)cb_ring_start(thread_cb);
  char *ring_end   = (char*)cb_ring_end(thread_cb);
  int ret;

  (void)ret;

  assert(pagesize > 0 && is_power_of_2(pagesize));
  assert((uintptr_t)ring_start % pagesize == 0);
  assert((uintptr_t)ring_end % pagesize == 0);

  //Unprotect the header
  for (char *m = (char*)thread_cb; m < ring_start; m += pagesize) {
    ret = mprotect(thread_cb, pagesize, PROT_READ | PROT_WRITE);
    assert(ret == 0);
  }

  //Unprotect the ring
  for (char *m = ring_start; m < ring_end; m += pagesize) {
    ret = mprotect(m, pagesize, PROT_READ | PROT_WRITE);
    assert(ret == 0);
  }

  //Unprotect the loop
  for (char *m = ring_end, *e = ring_end + cb_loop_size(thread_cb); m < e; m += pagesize) {
    ret = mprotect(m, pagesize, PROT_READ | PROT_WRITE);
    assert(ret == 0);
  }
}

static int gcnestlevel = 0;

void collectGarbage() {
  cb_offset_t rr_offset;
  RCBP<struct gc_request_response> rr;
  struct cb_region tmp_region;
  cb_offset_t gc_start_offset, gc_end_offset;
  int old_exec_phase;
  int ret;

  (void)ret;
  (void)gcnestlevel;
  (void)old_exec_phase;

  assert(on_main_thread);

  size_t bytes_allocated_before_gc = vm.bytesAllocated;

  long pagesize = sysconf(_SC_PAGESIZE);
  assert(pagesize > 0 && is_power_of_2(pagesize));

  cb_offset_t this_point_of_gc = cb_cursor(thread_cb);
  cb_offset_t new_lower_bound = this_point_of_gc;

  //NOTE: The loop here is to cover the exceedingly rare theoretical
  // circumstance that a gc_request_response be allocated to the same raw
  // address as the immediately prior one, making observation of its completion
  // by integrate_any_gc_response()/gc_await_response() impossible.  The body of
  // the loop should execute at most twice.
  do {
    //ret = cb_memalign(&thread_cb, &rr_offset, cb_alignof(struct gc_request_response), sizeof(struct gc_request_response));
    ret = cb_memalign(&thread_cb, &rr_offset, pagesize, sizeof(struct gc_request_response));  //pagesize alignment needed for mprotect().
    assert(ret == 0);
    rr = rr_offset;
  } while (rr.cp() == gc_last_processed_response);

  if (pinned_lower_bound != CB_NULL && cb_offset_cmp(pinned_lower_bound, new_lower_bound) == -1)
    new_lower_bound = pinned_lower_bound;

#ifdef DEBUG_TRACE_GC
  KLOX_TRACE("====== BEGIN GC %u nestlevel:%d, NEW_LOWER_BOUND:%ju, exec_phase:%d\n",
             gc_integration_epoch, gcnestlevel++, (uintmax_t)new_lower_bound, exec_phase);
  KLOX_TRACE_ONLY(printStateOfWorld("pre-gc"));
#endif

  old_exec_phase = exec_phase;

  // Create a new thread_region because we if we are collecting due to a
  // presently-in-flight allocation (as of this writing, always the case), we
  // would want it to complete at a higher offset than new_lower_bound.  We also
  // want all of the new A regions to also be beyond new_lower_bound.
  ret = logged_region_create(&thread_cb, &thread_region, 1, 1024 * 1024, 0);

  exec_phase = EXEC_PHASE_FREEZE_A_REGIONS;
  freezeARegions(new_lower_bound);

  //Mark start of the GC destination region.
  gc_start_offset = cb_cursor(thread_cb);

  exec_phase = EXEC_PHASE_PREPARE_REQUEST;
  memset(rr.mp(), 0, sizeof(struct gc_request_response));

  //Prepare request contents
  //rr->req.orig_cb  NOTE: this gets set last, down below, after all allocations.
  rr.mp()->req.new_lower_bound           = new_lower_bound;
  rr.mp()->req.bytes_allocated_before_gc = bytes_allocated_before_gc;
  rr.mp()->req.exec_phase                = exec_phase;

  // Prepare condensing objtable B+C
  {
    // Create region which will be used for a blank ObjTableLayer.
    assert(alignof(ObjTableSM) <= (size_t)pagesize);
    ret = logged_region_create(&thread_cb,
                               &tmp_region,
                               pagesize,
                               sizeof(ObjTableSM),
                               CB_REGION_FINAL);
    assert(ret == 0);
    rr.mp()->req.objtable_blank_region = tmp_region;
    assert(cb_offset_cmp(cb_region_start(&(rr.cp()->req.objtable_blank_region)), new_lower_bound) >= 0);

    // Create region which will be used for the firstlevel structure of the new-B ObjTableLayer's structmap.
    ret = logged_region_create(&thread_cb,
                               &tmp_region,
                               alignof(ObjTableSM),
                               sizeof(ObjTableSM),
                               CB_REGION_FINAL);
    assert(ret == 0);
    rr.mp()->req.objtable_firstlevel_new_region = tmp_region;
    assert(cb_offset_cmp(cb_region_start(&(rr.cp()->req.objtable_firstlevel_new_region)), new_lower_bound) >= 0);

    // Create region which will be used for the contents (nodes and entries) of the new-B ObjTableLayer's structmap.
    ret = logged_region_create(&thread_cb,
                               &tmp_region,
                               1,  //objtable_consolidation_size() pessimizes with worst-case alignment, so no need to provide any here.
                               objtable_consolidation_size(&thread_objtable),
                               CB_REGION_FINAL);
    assert(ret == 0);
    rr.mp()->req.objtable_new_region = tmp_region;
    assert(cb_offset_cmp(cb_region_start(&(rr.cp()->req.objtable_new_region)), new_lower_bound) >= 0);

    // Provide the B and C ObjTableLayers which will be consolidated.
    objtablelayer_assign(&(rr.mp()->req.objtable_b), &(thread_objtable.b));
    objtablelayer_assign(&(rr.mp()->req.objtable_c), &(thread_objtable.c));
  }

  // Prepare condensing tristack B+C
  size_t tristack_b_plus_c_size = sizeof(Value) * (vm.tristack.abi - vm.tristack.cbi);
  KLOX_TRACE("tristack_b_plus_c_size: %zd\n", tristack_b_plus_c_size);
  ret = logged_region_create(&thread_cb,
                             &tmp_region,
                             pagesize,
                             tristack_b_plus_c_size,
                             CB_REGION_FINAL);
  assert(ret == 0);
  rr.mp()->req.tristack_new_region = tmp_region;
  assert(cb_region_start(&(rr.cp()->req.tristack_new_region)) >= new_lower_bound);
  rr.mp()->req.tristack_abi        = vm.tristack.abi;
  rr.mp()->req.tristack_bbo        = vm.tristack.bbo;
  rr.mp()->req.tristack_bbi        = vm.tristack.bbi;
  rr.mp()->req.tristack_cbo        = vm.tristack.cbo;
  rr.mp()->req.tristack_cbi        = vm.tristack.cbi;
  rr.mp()->req.tristack_stackDepth = vm.tristack.stackDepth;

  // Prepare condensing triframes B+C
  size_t triframes_b_plus_c_size = sizeof(CallFrame) * (vm.triframes.abi - vm.triframes.cbi);
  KLOX_TRACE("triframes_b_plus_c_size: %zd\n", triframes_b_plus_c_size);
  ret = logged_region_create(&thread_cb,
                             &tmp_region,
                             pagesize,
                             triframes_b_plus_c_size,
                             CB_REGION_FINAL);
  assert(ret == 0);
  rr.mp()->req.triframes_new_region = tmp_region;
  assert(cb_region_start(&(rr.cp()->req.triframes_new_region)) >= new_lower_bound);
  rr.mp()->req.triframes_abi        = vm.triframes.abi;
  rr.mp()->req.triframes_bbo        = vm.triframes.bbo;
  rr.mp()->req.triframes_bbi        = vm.triframes.bbi;
  rr.mp()->req.triframes_cbo        = vm.triframes.cbo;
  rr.mp()->req.triframes_cbi        = vm.triframes.cbi;
  rr.mp()->req.triframes_frameCount = vm.triframes.frameCount;

  // Prepare condensing strings B+C
  size_t strings_b_size = cb_bst_size(thread_cb, vm.strings.root_b);
  size_t strings_c_size = cb_bst_size(thread_cb, vm.strings.root_c);
  KLOX_TRACE("strings_b_size: %zd, strings_c_size: %zd\n",
             strings_b_size, strings_c_size);
  ret = logged_region_create(&thread_cb,
                             &tmp_region,
                             pagesize,
                             strings_b_size + strings_c_size,
                             CB_REGION_FINAL);
  assert(ret == 0);
  rr.mp()->req.strings_new_region = tmp_region;
  assert(cb_region_start(&(rr.cp()->req.strings_new_region)) >= new_lower_bound);
  rr.mp()->req.strings_root_b = vm.strings.root_b;
  rr.mp()->req.strings_root_c = vm.strings.root_c;

  // Prepare condensing globals B+C
  size_t globals_b_size = cb_bst_size(thread_cb, vm.globals.root_b);
  size_t globals_c_size = cb_bst_size(thread_cb, vm.globals.root_c);
  KLOX_TRACE("globals_b_size: %zd, globals_c_size: %zd\n",
             globals_b_size, globals_c_size);
  ret = logged_region_create(&thread_cb,
                             &tmp_region,
                             pagesize,
                             globals_b_size + globals_c_size,
                             CB_REGION_FINAL);
  assert(ret == 0);
  rr.mp()->req.globals_new_region = tmp_region;
  assert(cb_region_start(&(rr.cp()->req.globals_new_region)) >= new_lower_bound);
  rr.mp()->req.globals_root_b = vm.globals.root_b;
  rr.mp()->req.globals_root_c = vm.globals.root_c;

  //Prepare graying of "init" string.
  rr.mp()->req.init_string = vm.initString.id();

  //Prepare graying of open upvalues.
  rr.mp()->req.open_upvalues = vm.openUpvalues.id();

  //Mark end of the GC destination region.
  cb_memalign(&thread_cb, &gc_end_offset, pagesize, 1);

  rr.mp()->req.gc_dest_region_start = gc_start_offset;
  rr.mp()->req.gc_dest_region_end = gc_end_offset;

#if KLOX_SYNC_GC
  //FIXME: Temporarily disabled now that ObjTable is stored within the CB and the PIN_SCOPE of the compilation phase causes huge CBs to be created.
  //mprotect_all_except_gc_region(thread_cb, rr_offset, gc_end_offset);
#endif //KLOX_SYNC_GC

  rr.mp()->req.orig_cb = thread_cb;  //NOTE: Finally set it just before we send, to ensure we're through all allocations.

#if KLOX_TRACE_ENABLE
  {
    size_t  source_size = (size_t)(this_point_of_gc - last_point_of_gc);
    size_t  dest_size   = (size_t)(gc_end_offset - gc_start_offset);
    ssize_t delta_size  = (ssize_t)dest_size - (ssize_t)source_size;
    double  delta_pct   = (double)delta_size / (double)source_size * 100.0;

    KLOX_TRACE("GC will consolidate source range [%ju, %ju) (%zu bytes) to destination range [%ju, %ju) (%zu bytes).  Delta: %zd bytes, %0.1f %%.\n",
        (uintmax_t)last_point_of_gc, (uintmax_t)this_point_of_gc, source_size,
        (uintmax_t)gc_start_offset, (uintmax_t)gc_end_offset, dest_size,
        delta_size, delta_pct);
  }
#endif  //KLOX_TRACE_ENABLE

  last_point_of_gc = this_point_of_gc;
  thread_cutoff_offset = new_lower_bound;

  gc_submit_request(rr.mp());

#if KLOX_SYNC_GC
  {
    struct gc_request_response *rr_returned = gc_await_response();
    assert(rr_returned == rr.cp());
    KLOX_TRACE("received GC response with new_lower_bound:%ju, orig_cb:%p\n", (uintmax_t)rr_returned->req.new_lower_bound, rr_returned->req.orig_cb);
    //FIXME: Temporarily disabled now that ObjTable is stored within the CB and the PIN_SCOPE of the compilation phase causes huge CBs to be created.
    //unmprotect_all(thread_cb);

    //Send in a copy of the response, because integration causes allocation
    //which can lead to clobbering of the old CB which contains rr_returned.
    struct gc_request_response rr_copied;
    rr_copied = *rr_returned;
    integrateGCResponse(&rr_copied);

    //Nonetheless, the original pointer handle is still appropriate to use for
    //determining the latest processed gc_request_response.
    KLOX_TRACE("setting gc_last_processed_response to %p\n", rr_returned);
    gc_last_processed_response = rr_returned;
  }
#else

#if PROVOKE_RESIZE_DURING_GC
  if (!resize_during_gc_already_provoked) {
    ret = cb_resize(&thread_cb, cb_ring_size(thread_cb) * 2);
    assert(ret == 0);
    resize_during_gc_already_provoked = true;
  }
#endif  //PROVOKE_RESIZE_DURING_GC

  exec_phase = old_exec_phase;

  tristack_recache(&(vm.tristack), thread_cb);
  triframes_recache(&(vm.triframes), thread_cb);
  vm.currentFrame = vm.triframes.currentFrame;
  triframes_ensureCurrentFrameIsMutable(&vm.triframes);

#endif //KLOX_SYNC_GC
}

void integrateGCResponse(struct gc_request_response *rr) {
  exec_phase = EXEC_PHASE_INTEGRATE_RESULT;

  // Handle the case where we are integrating a received GC response, but that
  // consolidation occurred targeting an older CB. (We have resized to a new
  // CB since invoking the GC.)  We must copy the old GC destination region to
  // our present CB.  This is a rare circumstance which can be exercised by
  // PROVOKE_RESIZE_DURING_GC, and which cannot occur when KLOX_SYNC_GC=1
  if (rr->req.orig_cb != thread_cb) {
    KLOX_TRACE("GC Response received after resize, cb_memcpy() from old_cb:%p to new_cb: %p, range [%ju,%ju)\n",
        rr->req.orig_cb, thread_cb, (uintmax_t)rr->req.gc_dest_region_start, (uintmax_t)rr->req.gc_dest_region_end);
    cb_memcpy(thread_cb, rr->req.gc_dest_region_start,
              rr->req.orig_cb, rr->req.gc_dest_region_start,
              (rr->req.gc_dest_region_end - rr->req.gc_dest_region_start));
  }

  // Cache the current frame's ip_offset, so that we'll later derive the correct
  // ip under revised objtable locations.
  size_t currentFrameIpOffset;
  {
    CallFrame* frame = triframes_currentFrame(&(vm.triframes));

    assert(!frame->has_ip_offset);
    assert(frame->functionP == frame->function.clip().cp());
    assert(frame->constantsValuesP == frame->functionP->chunk.constants.values.clp().cp());
    assert(frame->ip_root == frame->functionP->chunk.code.clp().cp());
    currentFrameIpOffset = frame->ip - frame->ip_root;
  }

  //Integrate condensed objtable.
  KLOX_TRACE("objtable C %ju -> %ju\n", (uintmax_t)thread_objtable.c.sm->root_node_offset, (uintmax_t)0);
  KLOX_TRACE("objtable B %ju -> %ju\n", (uintmax_t)thread_objtable.b.sm->root_node_offset, (uintmax_t)rr->resp.objtable_new_b.sm->root_node_offset);
  objtablelayer_init(&(thread_objtable.c), thread_cb, rr->resp.objtable_blank_firstlevel_offset);
  objtablelayer_assign(&(thread_objtable.b), &(rr->resp.objtable_new_b));
  thread_objtable_lower_bound = cb_region_start(&(rr->req.objtable_blank_region));
  assert(thread_objtable.b.sm->root_node_offset == CB_NULL || thread_objtable.b.sm->root_node_offset >= rr->req.new_lower_bound);
  assert(thread_objtable.a.sm->root_node_offset == CB_NULL || thread_objtable.a.sm->root_node_offset >= rr->req.new_lower_bound);

  //Integrate condensed tristack.
  KLOX_TRACE("before condensing tristack\n");
  KLOX_TRACE_ONLY(tristack_print(&(vm.tristack)));
  vm.tristack.cbo = CB_NULL;
  vm.tristack.cbi = 0;
  vm.tristack.bbo = rr->resp.tristack_new_bbo;
  assert(rr->resp.tristack_new_bbi == 0);
  vm.tristack.bbi = rr->resp.tristack_new_bbi;
  tristack_recache(&(vm.tristack), thread_cb);
  KLOX_TRACE("after condensing tristack\n");
  KLOX_TRACE_ONLY(tristack_print(&(vm.tristack)));
  //assert(vm.tristack.cbo >= rr->req.new_lower_bound);
  assert(vm.tristack.bbo >= rr->req.new_lower_bound);
  assert(vm.tristack.abo >= rr->req.new_lower_bound);

  //Integrate condensed triframes.
  KLOX_TRACE("before integrating triframes  abo: %ju, abi: %ju, bbo: %ju, bbi: %ju, cbo: %ju, cbi: %ju\n",
             (uintmax_t)vm.triframes.abo,
             (uintmax_t)vm.triframes.abi,
             (uintmax_t)vm.triframes.bbo,
             (uintmax_t)vm.triframes.bbi,
             (uintmax_t)vm.triframes.cbo,
             (uintmax_t)vm.triframes.cbi);
  KLOX_TRACE_ONLY(triframes_print(&(vm.triframes)));
  vm.triframes.cbo = CB_NULL;
  vm.triframes.cbi = 0;
  vm.triframes.bbo = rr->resp.triframes_new_bbo;
  vm.triframes.bbi = rr->resp.triframes_new_bbi;
  //assert(vm.triframes.cbo >= rr->req.new_lower_bound);
  assert(vm.triframes.bbo >= rr->req.new_lower_bound);
  assert(vm.triframes.abo >= rr->req.new_lower_bound);
  triframes_recache(&(vm.triframes), thread_cb);
  assert(on_main_thread);
  vm.currentFrame = vm.triframes.currentFrame;
  KLOX_TRACE("after integrating triframes  abo: %ju, abi: %ju, bbo: %ju, bbi: %ju, cbo: %ju, cbi: %ju\n",
             (uintmax_t)vm.triframes.abo,
             (uintmax_t)vm.triframes.abi,
             (uintmax_t)vm.triframes.bbo,
             (uintmax_t)vm.triframes.bbi,
             (uintmax_t)vm.triframes.cbo,
             (uintmax_t)vm.triframes.cbi);
  KLOX_TRACE_ONLY(triframes_print(&(vm.triframes)));
  triframes_ensureCurrentFrameIsMutable(&vm.triframes);
  KLOX_TRACE("after ensuring last frame is mutable: abo: %ju, abi: %ju, bbo: %ju, bbi: %ju, cbo: %ju, cbi: %ju\n",
             (uintmax_t)vm.triframes.abo,
             (uintmax_t)vm.triframes.abi,
             (uintmax_t)vm.triframes.bbo,
             (uintmax_t)vm.triframes.bbi,
             (uintmax_t)vm.triframes.cbo,
             (uintmax_t)vm.triframes.cbi);
  KLOX_TRACE_ONLY(triframes_print(&(vm.triframes)));

  //Recache the pointers of the current frame.
  {
    CallFrame *frame = triframes_currentFrame(&(vm.triframes));

    assert(!frame->has_ip_offset);
    frame->functionP = frame->function.clip().cp();
    frame->constantsValuesP = frame->functionP->chunk.constants.values.clp().cp();
    frame->ip_root = frame->functionP->chunk.code.clp().cp();
    frame->ip = frame->ip_root + currentFrameIpOffset;
  }

  //Integrate condensed strings.
  vm.strings.root_c = CB_BST_SENTINEL;
  vm.strings.root_b = rr->resp.strings_new_root_b;
  assert(vm.strings.root_b >= rr->req.new_lower_bound);
  assert(vm.strings.root_a >= rr->req.new_lower_bound);

  //Integrate condensed globals.
  vm.globals.root_c = CB_BST_SENTINEL;
  vm.globals.root_b = rr->resp.globals_new_root_b;
  assert(vm.globals.root_b >= rr->req.new_lower_bound);
  assert(vm.globals.root_a >= rr->req.new_lower_bound);

  // Collect the white objects.
  exec_phase = EXEC_PHASE_FREE_WHITE_SET;
  OID<struct sObj> white_list = rr->resp.white_list;
  // Take off white objects from the front of the vm.objects list.
  while (!white_list.is_nil()) {
    OID<Obj> unreached = white_list;
    white_list = white_list.clip().cp()->white_next;
    freeObject(unreached);
  }

  size_t advance_len = rr->req.new_lower_bound - cb_start(thread_cb);

#ifdef DEBUG_CLOBBER
  if (advance_len > 0) {
    //Clobber old contents.
#ifdef DEBUG_TRACE_GC
    KLOX_TRACE("clobbering range [%ju,%ju) of cb %p (size: %ju, start: %ju, cursor: %ju)\n",
               (uintmax_t)cb_start(thread_cb),
               (uintmax_t)rr->req.new_lower_bound,
               thread_cb,
               (uintmax_t)cb_ring_size(thread_cb),
               (uintmax_t)cb_start(thread_cb),
               (uintmax_t)cb_cursor(thread_cb));
#endif  //DEBUG_TRACE_GC
    cb_memset(thread_cb, cb_start(thread_cb), '@', advance_len);
  }
#endif  //DEBUG_CLOBBER

  KLOX_TRACE("cb_start_advance() by %ju bytes (from %ju to %ju)\n",
      (uintmax_t)advance_len,
      (uintmax_t)cb_start(thread_cb),
      (uintmax_t)(cb_start(thread_cb) + advance_len));
  cb_start_advance(thread_cb, advance_len);

  // Adjust the heap size based on live memory.
  vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;

  exec_phase = EXEC_PHASE_INTERPRET;

  gc_request_is_outstanding = false;

#ifdef DEBUG_TRACE_GC
  KLOX_TRACE_ONLY(printStateOfWorld("post-gc"));
  KLOX_TRACE("====== END GC %u collected %ld bytes (from %ld to %ld) next at %ld, nestlevel:%d, final datasize:%ju, exec_phase:%d\n",
             gc_integration_epoch, rr->req.bytes_allocated_before_gc - vm.bytesAllocated, rr->req.bytes_allocated_before_gc, vm.bytesAllocated,
             vm.nextGC, --gcnestlevel, (uintmax_t)cb_data_size(thread_cb), exec_phase);
#endif

  ++gc_integration_epoch;
}
