#ifndef klox_cb_integration_h
#define klox_cb_integration_h

#include <assert.h>

#include <cb.h>
#include <cb_objtable.h>
#include <cb_print.h>
#include <cb_ref.hpp>
#include <cb_region.h>
#include <cb_term.h>
#include <cb_thread.h>

#include <cb_structmap_amt.h>
#include "trace.h"

// VM thread state.
extern __thread struct ObjTable   thread_objtable;
extern __thread cb_offset_t       pinned_lower_bound;
extern __thread bool              on_main_thread;
extern __thread bool              can_print;
extern __thread unsigned int      gc_integration_epoch;
extern __thread cb_offset_t       thread_objtable_lower_bound;
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

static const int OBJTABLELAYER_FIRSTLEVEL_BITS = 10; //FIXME remove?
static const int FIELDS_FIRSTLEVEL_BITS = 0;
static const int METHODS_FIRSTLEVEL_BITS = 0;

typedef cb_structmap_amt<0, 5> MethodsSM;
typedef cb_structmap_amt<0, 5> FieldsSM;

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

int methods_layer_init(struct cb **cb, struct cb_region *region, MethodsSM *sm);
int fields_layer_init(struct cb **cb, struct cb_region *region, FieldsSM *sm);

size_t
klox_no_external_size(const struct cb      *cb,
                      const struct cb_term *term);

size_t
klox_no_external_size2(const struct cb *cb,
                       uint64_t         offset);

size_t
klox_allocation_size(const struct cb *cb,
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
