#ifndef klox_cb_integration_h
#define klox_cb_integration_h

#include <assert.h>

#include <cb.h>
#include <cb_print.h>
#include <cb_region.h>
#include <cb_term.h>

#include "structmap_amt.h"
#include "trace.h"

// VM thread state.
extern __thread struct cb        *thread_cb;
extern __thread struct cb_at_immed_param_t thread_cb_at_immed_param;
extern __thread struct cb_region  thread_region;
extern __thread cb_offset_t       thread_cutoff_offset;
extern __thread struct ObjTable   thread_objtable;
extern __thread cb_offset_t       pinned_lower_bound;
extern __thread bool              on_main_thread;
extern __thread bool              can_print;
extern __thread unsigned int      gc_integration_epoch;
extern __thread cb_offset_t       thread_objtable_lower_bound;
extern __thread unsigned int      addl_collision_nodes;
extern __thread unsigned int      snap_addl_collision_nodes;
extern __thread uintmax_t         thread_preserved_objects_count;
extern __thread uintmax_t         thread_new_objects_since_last_gc_count;

// GC thread state.
//FIXME make these to gc-thread-local.
extern struct cb_region  gc_thread_grayset_bst_region;
extern cb_offset_t       gc_thread_grayset_bst;
extern struct cb_region  gc_thread_dedupeset_bst_region;
extern cb_offset_t       gc_thread_dedupeset_bst;

extern struct gc_request_response* gc_last_processed_response;
extern bool gc_request_is_outstanding;

extern int exec_phase;
extern int gc_phase;
extern bool is_resizing;

#define CB_CACHE_LINE_SIZE 64
#define CB_NULL ((cb_offset_t)0)

#define ALREADY_WHITE_FLAG ((cb_offset_t)1)
#define ALREADY_WHITE(OFFSET) !!((OFFSET) & ALREADY_WHITE_FLAG)
#define PURE_OFFSET(OFFSET) ((OFFSET) & ~ALREADY_WHITE_FLAG)

static const int OBJTABLELAYER_FIRSTLEVEL_BITS = 10;
static const int FIELDS_FIRSTLEVEL_BITS = 0;
static const int METHODS_FIRSTLEVEL_BITS = 0;

typedef structmap_amt<19, 5> ObjTableSM;
typedef structmap_amt<0, 5> MethodsSM;
typedef structmap_amt<0, 5> FieldsSM;

#if NDEBUG
#define DEBUG_ONLY(x)
#else
#define DEBUG_ONLY(x) x
#endif

enum {
  EXEC_PHASE_COMPILE,
  EXEC_PHASE_INTERPRET,
  EXEC_PHASE_FREEZE_A_REGIONS,
  EXEC_PHASE_PREPARE_REQUEST,
  EXEC_PHASE_INTEGRATE_RESULT,
  EXEC_PHASE_FREE_WHITE_SET
};

enum {
  GC_PHASE_NORMAL_EXEC,
  GC_PHASE_RESET_GC_STATE,
  GC_PHASE_MARK_STACK_ROOTS,
  GC_PHASE_MARK_FRAMES_ROOTS,
  GC_PHASE_MARK_OPEN_UPVALUES,
  GC_PHASE_MARK_GLOBAL_ROOTS,
  GC_PHASE_MARK_ALL_LEAVES,
  GC_PHASE_CONSOLIDATE
};


struct scoped_pin
{
  const char  *func_;
  int          line_;
  cb_offset_t  prev_pin_offset_;
  cb_offset_t  curr_pin_offset_;

  scoped_pin(const char *func, int line);
  ~scoped_pin();
};

#define PIN_SCOPE scoped_pin _sp(__PRETTY_FUNCTION__, __LINE__)

struct cbp
{
  void        *pointer_;
  cb_offset_t  offset_;
  struct cb   *cb_;

  cbp(cb_offset_t offset)
    : pointer_(offset == CB_NULL ? NULL : cb_at_immed(&thread_cb_at_immed_param, offset)),
      offset_(offset),
      cb_(thread_cb)
  { }

  cbp(cb_offset_t offset, struct cb *cb)
    : pointer_(offset == CB_NULL ? NULL : cb_at(cb, offset)),
      offset_(offset),
      cb_(cb)
  { }

  cbp(cbp const &rhs)
    : pointer_(rhs.pointer_),
      offset_(rhs.offset_),
      cb_(rhs.cb_)
  {
    assert((offset_ == CB_NULL && pointer_ == NULL) || pointer_ == cb_at(cb_, offset_));
  }

  bool is_nil() {
    return !pointer_;
  }
};

template<typename T>
struct CBP : cbp
{
  CBP() : cbp(CB_NULL) { }

  CBP(cb_offset_t offset) : cbp(offset) { }

  CBP(cb_offset_t offset, struct cb *cb) : cbp(offset, cb) { }

  CBP(CBP<T> const &rhs) : cbp(rhs) { }

  CBP<T>& operator=(const CBP<T> &rhs) = default;

  const T* cp() {
    return static_cast<const T*>(pointer_);
  }

  T* mp() {
    return static_cast<T*>(pointer_);
  }
};


void rcbp_add(struct rcbp *item);
void rcbp_remove(struct rcbp *item);
void rcbp_rewrite_list(struct cb *new_cb);

struct rcbp : cbp
{
  rcbp *prev_;
  rcbp *next_;

  rcbp(cb_offset_t offset) : cbp(offset) { rcbp_add(this); }

  rcbp(cb_offset_t offset, struct cb *cb) : cbp(offset, cb) { rcbp_add(this); }

  rcbp(cbp const &rhs) : cbp(rhs) { rcbp_add(this); }

  ~rcbp() { rcbp_remove(this); }
};

template<typename T>
struct RCBP : rcbp
{
  RCBP() : rcbp(CB_NULL) { }

  RCBP(cb_offset_t offset) : rcbp(offset) { }

  RCBP(cb_offset_t offset, struct cb *cb) : rcbp(offset, cb) { }

  RCBP(RCBP<T> const &rhs) : rcbp(rhs) { }

  RCBP(cbp const &rhs) : rcbp(rhs) { }

  const T* cp() {
    return static_cast<const T*>(pointer_);
  }

  T* mp() {
    return static_cast<T*>(pointer_);
  }

  RCBP<T>& operator=(const RCBP<T> &rhs) {
    pointer_ = rhs.pointer_;
    offset_ = rhs.offset_;
    cb_ = rhs.cb_;
    return *this;
  }
};


template<typename T>
struct CBO
{
  cb_offset_t offset_;

  CBO() : offset_(CB_NULL) { }

  CBO(cb_offset_t offset) : offset_(offset) { }

  CBO(CBO<T> const &rhs) : offset_(rhs.offset_) { }

  constexpr CBO<T>& operator=(const CBO<T> &rhs) {
    offset_ = rhs.offset_;
    return *this;
  }

  bool is_nil() const {
    return (offset_ == CB_NULL);
  }

  //Underlying offset
  cb_offset_t co() const {
    return offset_;
  }

  //Underlying offset
  cb_offset_t mo() const {
    return offset_;
  }

  //Local dereference
  CBP<const T> clp() const {
    //return static_cast<const T*>(cb_at(thread_cb, offset_));
    return CBP<const T>(offset_);
  }

  //Local dereference
  CBP<T> mlp() {
    //return static_cast<T*>(cb_at(thread_cb, offset_));
    return CBP<T>(offset_);
  }

  //Remote dereference
  CBP<const T> crp(struct cb *remote_cb) const {
    return CBP<const T>(offset_, remote_cb);
  }

  //Remote dereference
  CBP<T> mrp(struct cb *remote_cb) const {
    return CBP<T>(offset_, remote_cb);
  }
};

typedef struct { uint64_t id; } ObjID;

typedef struct ObjTableLayer {
  cb_offset_t  sm_offset;
  ObjTableSM  *sm;
} ObjTableLayer;

typedef struct ObjTable {
  ObjTableLayer a;
  ObjTableLayer b;
  ObjTableLayer c;
  ObjID         next_obj_id;
} ObjTable;

int objtablelayer_init(ObjTableLayer *layer, struct cb *cb, cb_offset_t sm_offset);
void objtablelayer_recache(ObjTableLayer *layer, struct cb *cb);
int objtablelayer_assign(ObjTableLayer *dest, ObjTableLayer *src);

typedef int (*objtablelayer_traverse_func_t)(uint64_t key, uint64_t value, void *closure);
int objtablelayer_traverse(const struct cb                **cb,
                           ObjTableLayer                   *layer,
                           objtablelayer_traverse_func_t   func,
                           void                            *closure);

size_t objtablelayer_external_size(ObjTableLayer *layer);
size_t objtablelayer_internal_size(ObjTableLayer *layer);
size_t objtablelayer_size(ObjTableLayer *layer);
void objtablelayer_external_size_adjust(ObjTableLayer *layer, ssize_t adjustment);
// NOTE: The following is used to pre-align a region's cursor before an
// objtablelayer_insert() in copy_objtable_b() and copy_objtable_c_not_in_b()
// for the sake of accurately tracking in Debug builds how much of the region is
// being consumed by that insertion.
extern inline size_t objtablelayer_insertion_alignment_get() { return 8; }


extern inline int
objtablelayer_insert(struct cb        **cb,
                     struct cb_region  *region,
                     ObjTableLayer     *layer,
                     uint64_t           key,
                     uint64_t           value)
{
  assert(layer->sm == (ObjTableSM*)cb_at(thread_cb, layer->sm_offset));
  return layer->sm->insert(cb, region, key, value);
}

extern inline bool
objtablelayer_lookup(const struct cb *cb,
                     ObjTableLayer   *layer,
                     uint64_t         key,
                     uint64_t        *value)
{
  assert(layer->sm == (ObjTableSM*)cb_at(cb, layer->sm_offset));
  return (layer->sm->lookup(cb, key, value) && *value != CB_NULL);
}

int methods_layer_init(struct cb **cb, struct cb_region *region, MethodsSM *sm);
int fields_layer_init(struct cb **cb, struct cb_region *region, FieldsSM *sm);

void objtable_init(ObjTable *obj_table, struct cb *cb, cb_offset_t a_offset, cb_offset_t b_offset, cb_offset_t c_offset);
void objtable_recache(ObjTable *obj_table, struct cb *cb);
void objtable_add_at(ObjTable *obj_table, ObjID obj_id, cb_offset_t offset);
ObjID objtable_add(ObjTable *obj_table, cb_offset_t offset);
cb_offset_t objtable_lookup(ObjTable *obj_table, ObjID obj_id);
cb_offset_t objtable_lookup_A(ObjTable *obj_table, ObjID obj_id);
cb_offset_t objtable_lookup_B(ObjTable *obj_table, ObjID obj_id);
cb_offset_t objtable_lookup_C(ObjTable *obj_table, ObjID obj_id);
void objtable_invalidate(ObjTable *obj_table, ObjID obj_id);
void objtable_external_size_adjust_A(ObjTable *obj_table, ssize_t adjustment);
void objtable_freeze(ObjTable *obj_table, struct cb **cb, struct cb_region *region);
size_t objtable_consolidation_size(ObjTable *obj_table);


#define CB_NULL_OID ((ObjID) { 0 })

cb_offset_t resolveAsMutableLayer(ObjID objid);

template<typename T>
struct OID
{
  ObjID objid_;

  OID() : objid_(CB_NULL_OID) { }

  OID(ObjID objid) : objid_(objid) { }

  OID(OID<T> const &rhs) : objid_(rhs.objid_) { }

  constexpr OID<T>& operator=(const OID<T> &rhs) {
    objid_ = rhs.objid_;
    return *this;
  }

  bool is_nil() const {
    return (objid_.id == CB_NULL_OID.id);
  }

  bool is_valid() const {
    return (!is_nil() && co() != CB_NULL);
  }

  //Underlying id
  ObjID id() const {
    return objid_;
  }

  //Underlying offset
  cb_offset_t co() const {
    return objtable_lookup(&thread_objtable, objid_);
  }

  //Underlying offset (alternative objtable)
  cb_offset_t co_alt(ObjTable *ot) const {
    return objtable_lookup(ot, objid_);
  }

  //Underlying offset, if this ObjID exists in A region mapping. CB_NULL otherwise.
  cb_offset_t co_A() const {
    return objtable_lookup_A(&thread_objtable, objid_);
  }

  //Underlying offset, if this ObjID exists in A region mapping. CB_NULL otherwise.
  cb_offset_t co_B() const {
    return objtable_lookup_B(&thread_objtable, objid_);
  }

  //Underlying offset, if this ObjID exists in A region mapping. CB_NULL otherwise.
  cb_offset_t co_C() const {
    return objtable_lookup_C(&thread_objtable, objid_);
  }

  //Underlying offset
  cb_offset_t mo() const {
    return objtable_lookup(&thread_objtable, objid_);
  }

  //Local dereference
  CBP<const T> clip() const {
    return CBP<const T>(co());
  }

  //Remote dereference
  CBP<const T> crip(struct cb *remote_cb) const {
    return CBP<const T>(co(), remote_cb);
  }

  //Remote dereference, alternative objtable
  CBP<const T> crip_alt(struct cb *remote_cb, ObjTable *ot) const {
    return CBP<const T>(co_alt(ot), remote_cb);
  }

  //Local dereference, region A only
  CBP<const T> clipA() const {
    return CBP<const T>(co_A());
  }

  //Local dereference, region B only
  CBP<const T> clipB() const {
    return CBP<const T>(co_B());
  }

  //Local dereference, region C only
  CBP<const T> clipC() const {
    return CBP<const T>(co_C());
  }

  //Local mutable dereference
  CBP<T> mlip() {
    //assert(exec_phase == EXEC_PHASE_INTERPRET);
    assert(on_main_thread);
    return CBP<T>(resolveAsMutableLayer(objid_));
  }

  //Remote dereference
  //T* rp(struct cb *remote_cb) {
  //  return static_cast<T*>(cb_at(remote_cb, offset_));
  //}
};


size_t
klox_no_external_size(const struct cb      *cb,
                      const struct cb_term *term);

size_t
klox_no_external_size2(const struct cb *cb,
                       uint64_t         offset);

int
klox_obj_at_offset_deep_comparator(const struct cb *cb,
                                   const struct cb_term *lhs,
                                   const struct cb_term *rhs);

int
klox_value_deep_comparator(const struct cb *cb,
                           const struct cb_term *lhs,
                           const struct cb_term *rhs);

int
klox_value_shallow_comparator(const struct cb *cb,
                              const struct cb_term *lhs,
                              const struct cb_term *rhs);

int
klox_null_comparator(const struct cb *cb,
                     const struct cb_term *lhs,
                     const struct cb_term *rhs);

int
klox_value_render(cb_offset_t           *dest_offset,
                  struct cb            **cb,
                  const struct cb_term  *term,
                  unsigned int           flags);

size_t
klox_value_no_external_size(const struct cb      *cb,
                            const struct cb_term *term);

void
klox_on_cb_preresize(struct cb *old_cb, struct cb *new_cb);

void
klox_on_cb_resize(struct cb *old_cb, struct cb *new_cb);



struct gc_request
{
  struct cb        *orig_cb;
  cb_offset_t       gc_dest_region_start;
  cb_offset_t       gc_dest_region_end;
  cb_offset_t       new_lower_bound;
  size_t            bytes_allocated_before_gc;
  int               exec_phase;

  //Working areas for GC thread's gray list, gray set, and de-dupe set.
  struct cb_region  gc_gray_list_region;
  struct cb_region  gc_grayset_bst_region;
  struct cb_region  gc_dedupeset_bst_region;

  //Objtable
  struct cb_region  objtable_blank_region;
  struct cb_region  objtable_firstlevel_new_region;
  struct cb_region  objtable_new_region;
  ObjTableLayer     objtable_b;
  ObjTableLayer     objtable_c;

  //Tristack
  struct cb_region  tristack_new_region;
  unsigned int      tristack_abi; // A base index  (mutable region)
  cb_offset_t       tristack_bbo; // B base offset
  unsigned int      tristack_bbi; // B base index
  cb_offset_t       tristack_cbo; // C base offset
  unsigned int      tristack_cbi; // C base index (always 0, really)
  unsigned int      tristack_stackDepth;  // [0, stack_depth-1] are valid entries.

  //Triframes
  struct cb_region  triframes_new_region;
  unsigned int      triframes_abi; // A base index  (mutable region)
  cb_offset_t       triframes_bbo; // B base offset
  unsigned int      triframes_bbi; // B base index
  cb_offset_t       triframes_cbo; // C base offset
  unsigned int      triframes_cbi; // C base index (always 0, really)
  unsigned int      triframes_frameCount;  // [0, stack_depth-1] are valid entries.

  //NOTE: openUpvalues need no special handling, as they are simply a linked
  // list through OIDs in the objtable.  As long as they are grayed, they will
  // be consolidated through objtable consolidation.

  //Strings
  struct cb_region  strings_new_region;
  cb_offset_t       strings_root_b;
  cb_offset_t       strings_root_c;

  //Globals
  struct cb_region  globals_new_region;
  cb_offset_t       globals_root_b;
  cb_offset_t       globals_root_c;

  //"init" string
  ObjID             init_string;

  //Open upvalues
  ObjID             open_upvalues;
};

struct gc_response
{
  cb_offset_t   objtable_blank_firstlevel_offset;
  ObjTableLayer objtable_new_b;

  cb_offset_t  tristack_new_bbo; // B base offset
  unsigned int tristack_new_bbi; // B base index (always 0, really)

  cb_offset_t  triframes_new_bbo; // B base offset
  unsigned int triframes_new_bbi; // B base index (always 0, really)

  cb_offset_t  strings_new_root_b;

  cb_offset_t  globals_new_root_b;

  uintmax_t    preserved_objects_count;
  ObjID        white_list;
};

struct gc_request_response
{
  struct gc_request  req;
  struct gc_response resp;
};

int gc_init(void);
int gc_deinit(void);
void gc_submit_request(struct gc_request_response *request);
struct gc_request_response* gc_await_response(void);
void integrate_any_gc_response(void);

int gc_perform(struct gc_request_response *rr);

extern inline int
logged_region_create_(struct cb        **cb,
                      struct cb_region  *region,
                      size_t             alignment,
                      size_t             size,
                      unsigned int       flags,
                      const char        *fl,
                      int                line,
                      const char        *fun)
{
  int ret;

  ret = cb_region_create(cb, region, alignment, size, flags);
  if (ret == CB_SUCCESS) {
    KLOX_TRACE_PREFIXED(fl, line, fun, "region %p assigned offset range [%ju, %ju)\n",
               region, (uintmax_t)region->start, (uintmax_t)region->end);
  }

  return ret;
}

#define logged_region_create(A,B,C,D,E) \
  logged_region_create_(A,B,C,D,E,__FILE__,__LINE__,__FUNCTION__)


#endif
