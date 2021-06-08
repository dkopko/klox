#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cb_bst.h"

#include "cb_integration.h"
#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "vm.h"
#include "trace.h"
#if KLOX_ILAT
#include "cycle.h"

static struct {
  uint64_t count;
  uint64_t total_lat;
} lats[OP_METHOD+1];

#endif //KLOX_ILAT

static void
tristack_reset(TriStack *ts) {
  cb_offset_t new_offset;
  int ret;

  (void)ret;

  ret = cb_region_memalign(&thread_cb,
                           &thread_region,
                           &new_offset,
                           cb_alignof(Value),
                           sizeof(Value) * STACK_MAX);
  assert(ret == 0);

  ts->abo = new_offset;
  ts->abi = 0;
  ts->adirect = (Value*)cb_at_immed(thread_ring_start, thread_ring_mask, new_offset);
  ts->bbo = CB_NULL;
  ts->bbi = 0;
  ts->bdirect = 0;
  ts->cbo = CB_NULL;
  ts->cbi = 0;
  ts->cdirect = 0;
  ts->stackDepth = 0;
}

void tristack_recache(TriStack *ts, struct cb *target_cb) {
  ts->adirect = (ts->abo == CB_NULL ? 0 : (Value*)cb_at(target_cb, ts->abo));
  ts->bdirect = (ts->bbo == CB_NULL ? 0 : (Value*)cb_at(target_cb, ts->bbo));
  ts->cdirect = (ts->cbo == CB_NULL ? 0 : (Value*)cb_at(target_cb, ts->cbo));
}

static Value*
tristack_at_bc(TriStack *ts, unsigned int index) {
  assert(index < ts->stackDepth);
  //CBINT FIXME - add a way to check maximum index of B subsection?

  if (index >= ts->bbi) {
    Value *retval = &(ts->bdirect[index - ts->bbi]);
    assert(is_resizing || ts->bdirect == static_cast<Value*>(cb_at(thread_cb, ts->bbo)));
    assert(is_resizing || retval == static_cast<Value*>(cb_at(thread_cb, ts->bbo + (index - ts->bbi) * sizeof(Value))));
    return retval;
  } else {
    Value *retval = &(ts->cdirect[index - ts->cbi]);
    assert(is_resizing || ts->cdirect == static_cast<Value*>(cb_at(thread_cb, ts->cbo)));
    assert(is_resizing || retval == static_cast<Value*>(cb_at(thread_cb, ts->cbo + (index - ts->cbi) * sizeof(Value))));
    return retval;
  }
}

Value*
tristack_at(TriStack *ts, unsigned int index) {
  if (ts->stackDepth == 0)
    return NULL;

  assert(index <= ts->stackDepth);

  if (index >= ts->abi) {
    Value *retval = &(ts->adirect[index - ts->abi]);
    assert(is_resizing || ts->adirect == static_cast<Value*>(cb_at(thread_cb, ts->abo)));
    assert(is_resizing || retval == static_cast<Value*>(cb_at(thread_cb, ts->abo + (index - ts->abi) * sizeof(Value))));
    return retval;
  } else if (index >= ts->bbi) {
    Value *retval = &(ts->bdirect[index - ts->bbi]);
    assert(is_resizing || ts->bdirect == static_cast<Value*>(cb_at(thread_cb, ts->bbo)));
    assert(is_resizing || retval == static_cast<Value*>(cb_at(thread_cb, ts->bbo + (index - ts->bbi) * sizeof(Value))));
    return retval;
  } else {
    Value *retval = &(ts->cdirect[index - ts->cbi]);
    assert(is_resizing || ts->cdirect == static_cast<Value*>(cb_at(thread_cb, ts->cbo)));
    assert(is_resizing || retval == static_cast<Value*>(cb_at(thread_cb, ts->cbo + (index - ts->cbi) * sizeof(Value))));
    return retval;
  }
}

const char *
tristack_regionname_at(TriStack *ts, unsigned int index) {
  if (index >= ts->abi) return "A";
  if (index >= ts->bbi) return "B";
  return "C";
}

void
tristack_print(TriStack *ts) {
  KLOX_TRACE_("TRACE STACK ");
  for (unsigned int i = 0; i < vm.tristack.stackDepth; ++i) {
    KLOX_TRACE_("%d%s[ ", i, tristack_regionname_at(&(vm.tristack), i));
    KLOX_TRACE_ONLY(printValue(*tristack_at(&(vm.tristack), i), false));
    KLOX_TRACE_(" ] ");
  }
  KLOX_TRACE_("\n");
}

static void
triframes_reset(TriFrames *tf) {
  cb_offset_t new_offset;
  int ret;

  (void)ret;

  ret = cb_region_memalign(&thread_cb,
                           &thread_region,
                           &new_offset,
                           cb_alignof(CallFrame),
                           sizeof(CallFrame) * FRAMES_MAX);
  assert(ret == 0);

  tf->abo = new_offset;
  tf->abi = 0;
  tf->adirect = (CallFrame*)cb_at_immed(thread_ring_start, thread_ring_mask, new_offset);
  tf->bbo = CB_NULL;
  tf->bbi = 0;
  tf->bdirect = 0;
  tf->cbo = CB_NULL;
  tf->cbi = 0;
  tf->cdirect = 0;
  tf->frameCount = 0;
  tf->currentFrame = NULL;
}

void triframes_recache(TriFrames *tf, struct cb *target_cb) {
  tf->adirect = (tf->abo == CB_NULL ? 0 : (CallFrame*)cb_at(target_cb, tf->abo));
  tf->bdirect = (tf->bbo == CB_NULL ? 0 : (CallFrame*)cb_at(target_cb, tf->bbo));
  tf->cdirect = (tf->cbo == CB_NULL ? 0 : (CallFrame*)cb_at(target_cb, tf->cbo));

  unsigned int currentFrameIndex = triframes_frameCount(tf) - 1;

  //NOTE: Because this must point to target_cb, it is inappropriate to use
  // triframes_at() here, which is specialized toward thread_cb.
  if (currentFrameIndex >= tf->abi) {
    tf->currentFrame = &(tf->adirect[currentFrameIndex - tf->abi]);
  } else if (currentFrameIndex >= tf->bbi) {
    tf->currentFrame = &(tf->bdirect[currentFrameIndex - tf->bbi]);
  } else {
    tf->currentFrame = &(tf->cdirect[currentFrameIndex - tf->cbi]);
  }
}

extern inline void
triframes_enterFrame(TriFrames *tf) {
  assert(tf->frameCount >= tf->abi);

  assert(tf->adirect == static_cast<CallFrame*>(cb_at(thread_cb, tf->abo)));
  tf->currentFrame = &(tf->adirect[tf->frameCount - tf->abi]);
  ++(tf->frameCount);
}

void
triframes_ensureCurrentFrameIsMutable(TriFrames *tf) {
  CallFrame *newFrame;
  CallFrame *oldFrame;
  unsigned int currentFrameIndex;

  if (tf->frameCount == 0)
    return;

  currentFrameIndex = tf->frameCount - 1;

  if (currentFrameIndex >= tf->abi) {
    // Parent frame we are returning to is already in the mutable A section.
    assert(tf->adirect == static_cast<CallFrame*>(cb_at(thread_cb, tf->abo)));
    tf->currentFrame = &(tf->adirect[currentFrameIndex - tf->abi]);
    assert(on_main_thread);
    vm.currentFrame = tf->currentFrame;
    goto fixup_slots;
  }

  // Otherwise, current frame is in either the B or C read-only sections. It
  // must be copied to the mutable A section (will have destination of abo), and
  // abi adjustment must be made.
  if (currentFrameIndex >= tf->bbi) {
    assert(tf->bdirect == static_cast<CallFrame*>(cb_at(thread_cb, tf->bbo)));
    oldFrame = &(tf->bdirect[currentFrameIndex - tf->bbi]);
  } else {
    assert(tf->cdirect == static_cast<CallFrame*>(cb_at(thread_cb, tf->cbo)));
    oldFrame = &(tf->cdirect[currentFrameIndex - tf->cbi]);
  }
  assert(tf->adirect == static_cast<CallFrame*>(cb_at(thread_cb, tf->abo)));
  newFrame = tf->adirect;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
  memcpy(newFrame, oldFrame, sizeof(CallFrame));
#pragma GCC diagnostic pop
  tf->abi = currentFrameIndex;
  tf->currentFrame = newFrame;
  assert(on_main_thread);
  vm.currentFrame = tf->currentFrame;

  //NOTE: We must also ensure that the slots member of currentFrame
  // points to a contiguous array in the mutable A region.
fixup_slots:

  //Rederive cached pointers of the returned-to frame.
  if (__builtin_expect(!!(vm.currentFrame->gc_integration_epoch != gc_integration_epoch), 0)) {
    // Temporarily switch to holding an ip_offset.
    assert(!vm.currentFrame->has_ip_offset);
    vm.currentFrame->ip_offset = vm.currentFrame->ip - vm.currentFrame->ip_root;
    DEBUG_ONLY(vm.currentFrame->has_ip_offset = true);

    vm.currentFrame->functionP = vm.currentFrame->function.clip().cp();
    vm.currentFrame->constantsValuesP = vm.currentFrame->functionP->chunk.constants.values.clp().cp();
    vm.currentFrame->ip_root = vm.currentFrame->functionP->chunk.code.clp().cp();

    // Switch back to holding a raw ip pointer.
    assert(vm.currentFrame->has_ip_offset);
    vm.currentFrame->ip = vm.currentFrame->ip_root + vm.currentFrame->ip_offset;
    DEBUG_ONLY(vm.currentFrame->has_ip_offset = false);

    __builtin_prefetch(vm.currentFrame->ip);

    vm.currentFrame->gc_integration_epoch = gc_integration_epoch;
  }

  if (vm.currentFrame->slotsIndex < vm.tristack.abi) {
    Value *mutableRange = tristack_at(&(vm.tristack), vm.tristack.abi);
    for (int i = vm.tristack.stackDepth, e = vm.currentFrame->slotsIndex; i > e; i--) {
      mutableRange[(i-1) - vm.currentFrame->slotsIndex] = *tristack_at(&(vm.tristack), i-1);
    }
    vm.tristack.abi = vm.currentFrame->slotsIndex;
  }
  vm.currentFrame->slots = tristack_at(&(vm.tristack), vm.currentFrame->slotsIndex);
}

extern inline void
triframes_leaveFrame(TriFrames *tf) {
  assert(tf->frameCount > 0);
  --(tf->frameCount);
  triframes_ensureCurrentFrameIsMutable(tf);
}

CallFrame*
triframes_at(TriFrames *tf, unsigned int index) {
  CallFrame *retval;

  if (index >= tf->abi) {
    assert(tf->adirect == static_cast<CallFrame*>(cb_at(thread_cb, tf->abo)));
    retval = &(tf->adirect[index - tf->abi]);
  } else if (index >= tf->bbi) {
    assert(tf->bdirect == static_cast<CallFrame*>(cb_at(thread_cb, tf->bbo)));
    retval = &(tf->bdirect[index - tf->bbi]);
  } else {
    assert(tf->cdirect == static_cast<CallFrame*>(cb_at(thread_cb, tf->cbo)));
    retval = &(tf->cdirect[index - tf->cbi]);
  }

  return retval;
}

CallFrame*
triframes_at_alt(TriFrames *tf, unsigned int index, struct cb *target_cb)
{
  assert(index <= tf->frameCount);

  if (index >= tf->abi) {
    return static_cast<CallFrame*>(cb_at(target_cb, tf->abo + (index - tf->abi) * sizeof(CallFrame)));
  } else if (index >= tf->bbi) {
    return static_cast<CallFrame*>(cb_at(target_cb, tf->bbo + (index - tf->bbi) * sizeof(CallFrame)));
  } else {
    return static_cast<CallFrame*>(cb_at(target_cb, tf->cbo + (index - tf->cbi) * sizeof(CallFrame)));
  }
}

const char *
triframes_regionname_at(TriFrames *tf, unsigned int index) {
  if (index >= tf->abi) return "A";
  if (index >= tf->bbi) return "B";
  return "C";
}

void
printCallFrame(CallFrame *cf) {
  printf("ip:%p, ", cf->ip);
  //printf("si:%ju, ", (uintmax_t)cf->slotsIndex);
  //printf("sc:%ju, ", (uintmax_t)cf->slotsCount);
  printObject(cf->closure.id(), cf->closure.co(), (const Obj *)cf->closure.clip().cp(), false);
  printf(" | ");
  for (unsigned int i = 0; i < cf->slotsCount; ++i) {
    printf("%d%s[ ", cf->slotsIndex + i, tristack_regionname_at(&(vm.tristack), cf->slotsIndex + i));
    if (cf->slotsIndex + i < vm.tristack.stackDepth) {
      printValue(*tristack_at(&(vm.tristack), cf->slotsIndex + i), false);
    } else {
      //FIXME this sometimes occurs in printing and seems like not an error, but why?
      printf("BEYONDSTACK%d>=%d", cf->slotsIndex+i, vm.tristack.stackDepth);
    }
    printf(" ] ");
  }
}

void
triframes_print(TriFrames *tf) {
  KLOX_TRACE_("TRACE FRAMES ");
  for (unsigned int i = 0; i < vm.triframes.frameCount; ++i) {
    KLOX_TRACE_("%d%s{ ", i, triframes_regionname_at(&(vm.triframes), i));
    KLOX_TRACE_ONLY(printCallFrame(triframes_at(&(vm.triframes), i)));
    KLOX_TRACE_(" } ");
  }
  KLOX_TRACE_("\n");
}

VM vm;
GC gc;

static Value clockNative(int argCount, Value* args) {
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static void resetStack() {
  tristack_reset(&(vm.tristack));
  triframes_reset(&(vm.triframes));
  vm.openUpvalues = CB_NULL_OID;
}

static void runtimeError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  for (unsigned int i = triframes_frameCount(&(vm.triframes)); i > 0; --i) {
    CallFrame *frame = triframes_at(&(vm.triframes), i - 1);

    OID<ObjFunction> function = frame->closure.clip().cp()->function;
    // -1 because the IP is sitting on the next instruction to be
    // executed.
    size_t instruction;
    assert(!frame->has_ip_offset);
    instruction = frame->ip - frame->ip_root - 1;
    fprintf(stderr, "[line %d] in ",
            function.clip().cp()->chunk.lines.clp().cp()[instruction]);
    if (function.clip().cp()->name.is_nil()) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function.clip().cp()->name.clip().cp()->chars.clp().cp());
    }
  }

  resetStack();
}

static void defineNative(const char* name, NativeFn function) {
  PIN_SCOPE;

  OID<ObjString> nameOID = copyString(name, (int)strlen(name));
  Value nameVal = OBJ_VAL(nameOID.id());

  OID<ObjNative> nativeOID = newNative(function);
  Value nativeVal = OBJ_VAL(nativeOID.id());

  tableSet(&vm.globals, nameVal, nativeVal);
}

void initVM() {
  int ret;

  (void)ret;

  resetStack();
  vm.bytesAllocated = 0;
  vm.nextGC = 1024 * 1024;

  cb_offset_t a, blank;
  ret = cb_region_memalign(&thread_cb, &thread_region, &a, alignof(ObjTableSM), sizeof(ObjTableSM));
  assert(ret == CB_SUCCESS);
  ret = cb_region_memalign(&thread_cb, &thread_region, &blank, alignof(ObjTableSM), sizeof(ObjTableSM));
  assert(ret == CB_SUCCESS);
  objtable_init(&thread_objtable, thread_cb, a, blank, blank);

  initTable(&vm.globals, &klox_value_shallow_comparator, &klox_value_render);
  initTable(&vm.strings, &klox_value_deep_comparator, &klox_value_render);

  vm.initString = copyString("init", 4);

  defineNative("clock", clockNative);
}

void freeVM() {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  vm.initString = CB_NULL_OID;
}

extern inline Value peek(int distance) {
  return tristack_peek(&(vm.tristack), distance);
}

static bool call(OID<ObjClosure> closure, int argCount) {
  CallFrame* frame;
  OID<ObjFunction> function = closure.clip().cp()->function;
  const ObjFunction *functionP = function.clip().cp();

  if (argCount != functionP->arity) {
    runtimeError("Expected %d arguments but got %d.", functionP->arity, argCount);
    return false;
  }

  if (triframes_frameCount(&(vm.triframes)) == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  triframes_enterFrame(&(vm.triframes));
  frame = triframes_currentFrame(&(vm.triframes));
  frame->closure = closure;
  frame->function = function;
  frame->functionP = functionP;
  frame->constantsValuesP = functionP->chunk.constants.values.clp().cp();
  frame->ip_root = functionP->chunk.code.clp().cp();
  frame->ip = frame->ip_root;
  DEBUG_ONLY(frame->has_ip_offset = false);
  frame->gc_integration_epoch = gc_integration_epoch;

  // +1 to include either the called function or the receiver.
  frame->slotsCount = argCount + 1;
  frame->slotsIndex = vm.tristack.stackDepth - frame->slotsCount;
  frame->slots = tristack_at(&(vm.tristack), frame->slotsIndex);
  assert(frame->slots >= cb_at(thread_cb, vm.tristack.abo));  //Slots must be contiguous in mutable section A.
  return true;
}

static bool instanceFieldGet(OID<ObjInstance> instance, Value key, Value *value) {
  const ObjInstance *inst;  //cb-resize-safe (no allocations in lifetime)
  uint64_t k = AS_OBJ_ID(key).id;
  uint64_t v;

  if (((inst = instance.clipA().cp()) && inst->fields_sm.lookup(thread_cb, k, &v))
      || ((inst = instance.clipB().cp()) && inst->fields_sm.lookup(thread_cb, k, &v))
      || ((inst = instance.clipC().cp()) && inst->fields_sm.lookup(thread_cb, k, &v)))
  {
    value->val = v;
    return true;
  }

  return false;
}

static void instanceFieldSet(OID<ObjInstance> instance, Value key, Value value) {
  assert(IS_OBJ(key));

  //FIXME These RCBP's are safe over resizes, but are they safe over potential GCs caused by allocations below?
  RCBP<ObjInstance> instanceA = instance.mlip();
  RCBP<ObjInstance> instanceB = instance.clipB();
  RCBP<ObjInstance> instanceC = instance.clipC();
  uint64_t k = AS_OBJ_ID(key).id;
  uint64_t v = value.val;  //BIG FIXME - structmap won't handle double encodings of 0x0 and 0x1.
  int ret;

  (void)ret;

  //NOTE: To pass size validations done in Debug modes, we need to temporarily
  // oversize the objtable, as the update of the size of the fields_sm this
  // traversal is used for is not atomic w.r.t. the update of the size of the
  // objtable which contains it.
  KLOX_TRACE_ONLY(objtable_external_size_adjust_A(&thread_objtable,
                                                  FieldsSM::MODIFICATION_MAX_SIZE));

  size_t size_before = instanceA.cp()->fields_sm.size();
  unsigned int nodes_before = instanceA.cp()->fields_sm.node_count();

  FieldsSM fields_sm = instanceA.mp()->fields_sm;

  ret = fields_sm.insert(&thread_cb,
                         &thread_region,
                         k,
                         v);
  assert(ret == 0);

  instanceA.mp()->fields_sm = fields_sm;

  size_t size_after = instanceA.cp()->fields_sm.size();
  unsigned int nodes_after = instanceA.cp()->fields_sm.node_count();

  //NOTE: Because this field addition is done to an ObjInstance already present
  // in the objtable, we must manually inform the objtable of this independent
  // mutation of external size.
  objtable_external_size_adjust_A(&thread_objtable,
                                  (ssize_t)size_after - (ssize_t)size_before);

  KLOX_TRACE_ONLY(objtable_external_size_adjust_A(&thread_objtable,
                                                  - (ssize_t)FieldsSM::MODIFICATION_MAX_SIZE));

  //Account for future structmap enlargement on merge due to slot collisions.
  assert(nodes_after >= nodes_before);
  unsigned int delta_node_count = nodes_after - nodes_before;
  unsigned int b_collide_node_count = (instanceB.is_nil() ? 0 :
    instanceB.cp()->fields_sm.would_collide_node_count(thread_cb, k));
  unsigned int c_collide_node_count = (instanceC.is_nil() ? 0 :
    instanceC.cp()->fields_sm.would_collide_node_count(thread_cb, k));
  unsigned int max_collide_node_count = (b_collide_node_count > c_collide_node_count ? b_collide_node_count: c_collide_node_count);
  if (max_collide_node_count > delta_node_count) {
    unsigned int addl_node_count = max_collide_node_count - delta_node_count;
    KLOX_TRACE("Need addl_nodes (instance): %ju\n", (uintmax_t)addl_node_count);
    addl_collision_nodes += addl_node_count;
  }
}

static bool classMethodGet(OID<ObjClass> klass, Value key, Value *value) {
  const ObjClass *clazz;  //cb-resize-safe (no allocations in lifetime)
  uint64_t k = AS_OBJ_ID(key).id;
  uint64_t v;

  if (((clazz = klass.clipA().cp()) && clazz->methods_sm.lookup(thread_cb, k, &v))
      || ((clazz = klass.clipB().cp()) && clazz->methods_sm.lookup(thread_cb, k, &v))
      || ((clazz = klass.clipC().cp()) && clazz->methods_sm.lookup(thread_cb, k, &v)))
  {
    value->val = v;
    return true;
  }

  return false;
}

static void classMethodSet(OID<ObjClass> klass, Value key, Value value) {
  assert(IS_OBJ(key));
  assert(IS_OBJ(value));

  //FIXME These RCBP's are safe over resizes, but are they safe over potential GCs caused by allocations below?
  RCBP<ObjClass> classA = klass.mlip();
  RCBP<ObjClass> classB = klass.clipB();
  RCBP<ObjClass> classC = klass.clipC();
  uint64_t k = AS_OBJ_ID(key).id;
  uint64_t v = value.val;  //BIG FIXME - structmap won't handle double encodings of 0x0 and 0x1.
  int ret;

  (void)ret;

  //NOTE: To pass size validations done in Debug modes, we need to temporarily
  // oversize the objtable, as the update of the size of the methods_bst this
  // traversal is used for is not atomic w.r.t. the update of the size of the
  // objtable which contains it.
  KLOX_TRACE_ONLY(objtable_external_size_adjust_A(&thread_objtable,
                                                  MethodsSM::MODIFICATION_MAX_SIZE));

  size_t size_before = classA.cp()->methods_sm.size();
  size_t nodes_before = classA.cp()->methods_sm.node_count();

  MethodsSM methods_sm = classA.mp()->methods_sm;

  ret = methods_sm.insert(&thread_cb,
                          &thread_region,
                          k,
                          v);
  assert(ret == 0);

  classA.mp()->methods_sm = methods_sm;

  size_t size_after = classA.cp()->methods_sm.size();
  size_t nodes_after = classA.cp()->methods_sm.node_count();

  //NOTE: Because this method addition is done to an ObjClass already present
  // in the objtable, we must manually inform the objtable of this independent
  // mutation of external size.
  objtable_external_size_adjust_A(&thread_objtable,
                                  size_after - size_before);

  KLOX_TRACE_ONLY(objtable_external_size_adjust_A(&thread_objtable,
                                                  - (ssize_t)MethodsSM::MODIFICATION_MAX_SIZE));

  //Account for future structmap enlargement on merge due to slot collisions.
  assert(nodes_after >= nodes_before);
  unsigned int delta_node_count = nodes_after - nodes_before;
  unsigned int b_collide_node_count = (classB.is_nil() ? 0 :
    classB.cp()->methods_sm.would_collide_node_count(thread_cb, k));
  unsigned int c_collide_node_count = (classC.is_nil() ? 0 :
    classC.cp()->methods_sm.would_collide_node_count(thread_cb, k));
  unsigned int max_collide_node_count = (b_collide_node_count > c_collide_node_count ? b_collide_node_count : c_collide_node_count);
  if (max_collide_node_count > delta_node_count) {
    unsigned int addl_node_count = max_collide_node_count - delta_node_count;
    KLOX_TRACE("Need addl_nodes (class): %ju\n", (uintmax_t)addl_node_count);
    addl_collision_nodes += addl_node_count;
  }
}

static int
structmapTraversalMethodsAdd(uint64_t k, uint64_t v, void *closure)
{
  MethodsSM *dest_sm = (MethodsSM *)closure;
  int ret;

  (void)ret;

  //NOTE: To pass size validations done in Debug modes, we need to temporarily
  // oversize the objtable, as the update of the size of the methods bst this
  // traversal is used for is not atomic w.r.t. the update of the size of the
  // objtable which contains it.
  KLOX_TRACE_ONLY(objtable_external_size_adjust_A(&thread_objtable,
                                                  MethodsSM::MODIFICATION_MAX_SIZE));

  size_t size_before = dest_sm->size();

  ret = dest_sm->insert(&thread_cb,
                        &thread_region,
                        k,
                        v);
  assert(ret == 0);

  size_t size_after = dest_sm->size();
  objtable_external_size_adjust_A(&thread_objtable,
                                  (ssize_t)size_after - (ssize_t)size_before);

  KLOX_TRACE_ONLY(objtable_external_size_adjust_A(&thread_objtable,
                                                  - (ssize_t)MethodsSM::MODIFICATION_MAX_SIZE));
  return 0;
}

static void classMethodsAddAll(OID<ObjClass> subclassOID, OID<ObjClass> superclassOID) {
  //FIXME this method used to have a resize-under-traversal issue which may still exist for bst traversals elsewhere

  RCBP<ObjClass> subclass = subclassOID.mlip();
  MethodsSM superclass_methods_sm_tmp = superclassOID.clip().cp()->methods_sm;
  MethodsSM subclass_methods_sm_tmp = subclass.cp()->methods_sm;
  int ret;

  (void)ret;

  ret = superclass_methods_sm_tmp.traverse((const struct cb **)&thread_cb,
                                           &structmapTraversalMethodsAdd,
                                           &subclass_methods_sm_tmp);
  assert(ret == 0);

  subclass.mp()->methods_sm = subclass_methods_sm_tmp;
}

static bool callValue(Value callee, int argCount) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_BOUND_METHOD: {
        OID<ObjBoundMethod> bound = AS_BOUND_METHOD_OID(callee);
        const ObjBoundMethod *b = bound.clip().cp();

        // Replace the bound method with the receiver so it's in the
        // right slot when the method is called.
        Value* loc = tristack_at(&(vm.tristack), vm.tristack.stackDepth - (argCount + 1));
        assert(loc >= cb_at(thread_cb, vm.tristack.abo));  // Must be in the mutable section A.
        *loc = b->receiver;
        return call(b->method, argCount);
      }

      case OBJ_CLASS: {
        OID<ObjClass> klass = AS_CLASS_OID(callee);

        // Create the instance.
        Value tmp = OBJ_VAL(newInstance(klass).id());
        Value* loc = tristack_at(&(vm.tristack), vm.tristack.stackDepth - (argCount + 1));
        assert(loc >= cb_at(thread_cb, vm.tristack.abo));  // Must be in the mutable section A.
        *loc = tmp;
        Value initializer;
        if (classMethodGet(klass, OBJ_VAL(vm.initString.id()), &initializer)) {
          return call(AS_CLOSURE_OID(initializer), argCount);
        } else if (argCount != 0) {
          runtimeError("Expected 0 arguments but got %d.", argCount);
          return false;
        }

        return true;
      }

      case OBJ_CLOSURE:
        return call(AS_CLOSURE_OID(callee), argCount);

      case OBJ_NATIVE: {
        NativeFn native = AS_NATIVE(callee);
        Value* loc = tristack_at(&(vm.tristack), vm.tristack.stackDepth - argCount);
        assert(loc >= cb_at(thread_cb, vm.tristack.abo));
        Value result = native(argCount, loc);
        tristack_discardn(&(vm.tristack), argCount + 1);
        push(result);
        return true;
      }

      default:
        // Do nothing.
        break;
    }
  }

  runtimeError("Can only call functions and classes.");
  return false;
}

static bool invokeFromClass(OID<ObjClass> klass, Value name,
                            int argCount) {
  assert(IS_STRING(name));
  // Look for the method.
  Value method;
  if (!classMethodGet(klass, name, &method)) {
    OID<ObjString> nameOID = AS_STRING_OID(name);
    runtimeError("Undefined property '%s'.", nameOID.clip().cp()->chars.clp().cp());
    return false;
  }

  return call(AS_CLOSURE_OID(method), argCount);
}

static bool invoke(Value name, int argCount) {
  assert(IS_STRING(name));
  Value receiver = peek(argCount);

  if (!IS_INSTANCE(receiver)) {
    runtimeError("Only instances have methods.");
    return false;
  }

  OID<ObjInstance> instance = AS_INSTANCE_OID(receiver);

  // First look for a field which may shadow a method.
  Value value;
  if (instanceFieldGet(instance, name, &value)) {
    Value *loc = tristack_at(&(vm.tristack), vm.tristack.stackDepth - (argCount + 1));
    assert(loc >= cb_at(thread_cb, vm.tristack.abo));
    *loc = value;
    return callValue(value, argCount);
  }

  return invokeFromClass(instance.clip().cp()->klass, name, argCount);
}

static bool bindMethod(OID<ObjClass> klass, Value name) {
  assert(IS_STRING(name));
  Value method;
  if (!classMethodGet(klass, name, &method)) {
    OID<ObjString> nameOID = AS_STRING_OID(name);
    runtimeError("Undefined property '%s'.", nameOID.clip().cp()->chars.clp().cp());
    return false;
  }

  OID<ObjBoundMethod> bound = newBoundMethod(peek(0), AS_CLOSURE_OID(method));
  pop(); // Instance.
  push(OBJ_VAL(bound.id()));
  return true;
}

// Captures the local variable [local] into an [Upvalue]. If that local
// is already in an upvalue, the existing one is used. (This is
// important to ensure that multiple closures closing over the same
// variable actually see the same variable.) Otherwise, it creates a
// new open upvalue and adds it to the VM's list of upvalues.
// The passed-in stackIndex is in the domain of the full stack (and not just
// an offset within the frame's region of the stack).
static OID<ObjUpvalue> captureUpvalue(unsigned int stackIndex) {
  // If there are no open upvalues at all, we must need a new one.
  if (vm.openUpvalues.is_nil()) {
    vm.openUpvalues = newUpvalue(stackIndex);
    return vm.openUpvalues;
  }

  OID<ObjUpvalue> prevUpvalue = CB_NULL_OID;
  OID<ObjUpvalue> upvalue = vm.openUpvalues;

  // Walk towards the tail of the openUpvalues linked list until we find a
  // previously existing upvalue or reach where it should be.  The elements
  // of the list are ordered by decreasing stack indices, and we need to
  // maintain that order.
  const ObjUpvalue *upvalueP;
  while (!upvalue.is_nil()) {
    upvalueP = upvalue.clip().cp();
    if (upvalueP->valueStackIndex <= (int)stackIndex) { break; }

    prevUpvalue = upvalue;
    upvalue = upvalueP->next;
  }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
  // If we found it, reuse it.
  if (!upvalue.is_nil() && upvalueP->valueStackIndex == (int)stackIndex)
    return upvalue;
#pragma GCC diagnostic pop

  // We (have just) walked past the location on the linked list which should
  // have held an upvalue for this local, and we looked for it in the entry
  // we're at without finding it.  So there must not already be an upvalue
  // for this stack index. Make a new upvalue and link it in here to maintain
  // the list order of "openUpvalue stack indices decrease towards the tail of
  // the list".
  OID<ObjUpvalue> createdUpvalue = newUpvalue(stackIndex);
  createdUpvalue.mlip().mp()->next = upvalue;

  if (prevUpvalue.is_nil()) {
    // The new one is the first one in the list.
    vm.openUpvalues = createdUpvalue;
  } else {
    push(OBJ_VAL(createdUpvalue.id())); // Protect createdUpvalue from GC during mlip() allocation.

    prevUpvalue.mlip().mp()->next = createdUpvalue;

    pop();
  }

  return createdUpvalue;
}

static void closeUpvalues(unsigned int lastStackIndex) {
  //NOTE: The pointer comparison of vm.openValues.lp()->valueStackIndex vs.
  // 'lastStackIndex' works because lastStackIndex must exist within the stack.
  // The intent of this function is to take every upvalue of stack positions
  // greater-than-or-equal-to in index than 'lastStackIndex' and turn them into
  // "closed" upvalues, which hold a value in their own local storage instead
  // of referring to stack indices.  The 'lastStackIndex' argument which is
  // passed is the index of the first slot of a frame which is being exited (in
  // the case of an OP_RETURN for leaving functions), or the index of the
  // last element of the stack (in the case of OP_CLOSE_UPVALUE for leaving
  // scopes).

  while (!vm.openUpvalues.is_nil()) {
    OID<ObjUpvalue> upvalue = vm.openUpvalues;
    const ObjUpvalue *upvalueP = upvalue.clip().cp();
    if (upvalueP->valueStackIndex < (int)lastStackIndex) { break; }

    OID<ObjUpvalue> nextUpvalue = upvalueP->next;

    // Move the value into the upvalue itself.
    Value v = *tristack_at(&(vm.tristack), upvalueP->valueStackIndex);

    ObjUpvalue *mupvalue = upvalue.mlip().mp();  //cb-resize-safe (no allocations in lifetime)
    mupvalue->closed = v;
    mupvalue->valueStackIndex = -1;

    // Pop it off the open upvalue list
    vm.openUpvalues = nextUpvalue;
  }
}

static void defineMethod(Value name) {
  assert(IS_STRING(name));
  Value method = peek(0);
  OID<ObjClass> klass = AS_CLASS_OID(peek(1));
  classMethodSet(klass, name, method);
  pop();
}

static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate() {
  PIN_SCOPE;

  OID<ObjString> b = AS_STRING_OID(peek(0));
  OID<ObjString> a = AS_STRING_OID(peek(1));

  int length = a.clip().cp()->length + b.clip().cp()->length;
  CBO<char> /*char[]*/ chars = ALLOCATE(char, length + 1);
  const ObjString *aP = a.clip().cp();
  const ObjString *bP = b.clip().cp();
  memcpy(chars.mlp().mp(), aP->chars.clp().cp(), aP->length);
  memcpy(chars.mlp().mp() + aP->length, bP->chars.clp().cp(), bP->length);
  chars.mlp().mp()[length] = '\0';

  OID<ObjString> result = takeString(chars, length);
  pop();
  pop();
  push(OBJ_VAL(result.id()));
}

#define READ_BYTE() (*vm.currentFrame->ip++)
#define READ_SHORT() \
    (vm.currentFrame->ip += 2, (uint16_t)((vm.currentFrame->ip[-2] << 8) | vm.currentFrame->ip[-1]))
#define READ_CONSTANT() \
    (vm.currentFrame->constantsValuesP[READ_BYTE()])

static void perform_OP_CLOSURE() {
  //NOTE: This has been factored out of the switch statement for the use of
  //alloca() which tested faster than a dynamically sized tmp[upvalueCount].
  OID<ObjFunction> function = AS_FUNCTION_OID(READ_CONSTANT());

  // Create the closure and push it on the stack before creating
  // upvalues so that it doesn't get collected.
  OID<ObjClosure> closure = newClosure(function);
  push(OBJ_VAL(closure.id()));

  // Capture upvalues.
  // FIXME cache vm.currentFrame->closure.clip().cp()->upvalues.clp().cp() after reserving upvalueCount * sizeof(OID<ObjUpvalue>) (to prevent resizes due to captureUpvalue()).
  int upvalueCount = closure.clip().cp()->upvalueCount;
  size_t tmpSize = upvalueCount * sizeof(OID<ObjUpvalue>);
  OID<ObjUpvalue> *tmp = (OID<ObjUpvalue>*)alloca(tmpSize);
  for (int i = 0; i < upvalueCount; ++i) {
    uint8_t isLocal = READ_BYTE();
    uint8_t index = READ_BYTE();  // an index within the present frame's slots
    if (isLocal) {
      // Make an new upvalue to close over the parent's local variable.
      tmp[i] = captureUpvalue(vm.currentFrame->slotsIndex + index);
    } else {
      // Use the same upvalue as the current call frame.
      tmp[i] = vm.currentFrame->closure.clip().cp()->upvalues.clp().cp()[index];
    }
  }
  OID<ObjUpvalue> *dest = closure.mlip().mp()->upvalues.mlp().mp();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
  memcpy(dest, tmp, tmpSize);
#pragma GCC diagnostic pop
}

static InterpretResult run() {
  assert(on_main_thread);
  vm.currentFrame = triframes_currentFrame(&(vm.triframes));

#define BINARY_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
        runtimeError("Operands must be numbers."); \
        return INTERPRET_RUNTIME_ERROR; \
      } \
      \
      double b = AS_NUMBER(pop()); \
      double a = AS_NUMBER(pop()); \
      push(valueType(a op b)); \
    } while (false)

  KLOX_TRACE_ONLY(static int instruction_count = 0);
  for (;;) {

    assert(vm.currentFrame == triframes_currentFrame(&(vm.triframes)));
    assert(!vm.currentFrame->has_ip_offset);
    assert(vm.currentFrame->ip_root == vm.currentFrame->closure.clip().cp()->function.clip().cp()->chunk.code.clp().cp());
    assert((char*)vm.currentFrame->ip >= (char*)vm.currentFrame->ip_root);

    KLOX_TRACE("DANDEBUG instcount %ju %ju\n", (uintmax_t)instruction_count, (uintmax_t)*(vm.currentFrame->ip));
    KLOX_TRACE("DANDEBUG instoffset %jd\n", (intmax_t)(vm.currentFrame->ip - vm.currentFrame->ip_root));
    KLOX_TRACE_ONLY(instruction_count++);

#ifdef DEBUG_TRACE_EXECUTION
    tristack_print(&(vm.tristack));
    triframes_print(&(vm.triframes));

    disassembleInstruction(&vm.currentFrame->closure.clip().cp()->function.clip().cp()->chunk,
        (int)(vm.currentFrame->ip - vm.currentFrame->ip_root));

    assert(vm.currentFrame->slots == tristack_at(&(vm.tristack), vm.currentFrame->slotsIndex));
    assert(vm.currentFrame->slotsIndex >= vm.tristack.abi);
#endif

    uint8_t instruction;
#if KLOX_ILAT
    ticks t0 = getticks();
#endif //KLOX_ILAT
    switch (instruction = READ_BYTE()) {
      case OP_CONSTANT: {
        Value constant = READ_CONSTANT();
        push(constant);
        break;
      }
      case OP_NIL: push(NIL_VAL); break;
      case OP_TRUE: push(BOOL_VAL(true)); break;
      case OP_FALSE: push(BOOL_VAL(false)); break;

      case OP_POP: pop(); break;

      case OP_GET_LOCAL: {
        uint8_t slot = READ_BYTE();
        push(vm.currentFrame->slots[slot]);
        break;
      }

      case OP_SET_LOCAL: {
        uint8_t slot = READ_BYTE();
        vm.currentFrame->slots[slot] = peek(0);
        break;
      }

      case OP_GET_GLOBAL: {
        Value name = READ_CONSTANT();
        Value value;
        assert(IS_STRING(name));
        if (!tableGet(&vm.globals, name, &value)) {
          OID<ObjString> nameOID = AS_STRING_OID(name);
          runtimeError("Undefined variable '%s'.", nameOID.clip().cp()->chars.clp().cp());
          return INTERPRET_RUNTIME_ERROR;
        }
        push(value);
        break;
      }

      case OP_DEFINE_GLOBAL: {
        Value name = READ_CONSTANT();
        assert(IS_STRING(name));
        tableSet(&vm.globals, name, peek(0));
        pop();
        break;
      }

      case OP_SET_GLOBAL: {
        Value name = READ_CONSTANT();
        assert(IS_STRING(name));
        if (tableSet(&vm.globals, name, peek(0))) {
          tableDelete(&vm.globals, name);
          OID<ObjString> nameOID = AS_STRING_OID(name);
          runtimeError("Undefined variable '%s'.", nameOID.clip().cp()->chars.clp().cp());
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }

      case OP_GET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        const ObjUpvalue* upvalue = vm.currentFrame->closure.clip().cp()->upvalues.clp().cp()[slot].clip().cp();  //cb-resize-safe (no allocations in lifetime)
        if (upvalue->valueStackIndex == -1) {
          push(upvalue->closed);
        } else {
          push(*tristack_at(&(vm.tristack), upvalue->valueStackIndex));
        }
        break;
      }

      case OP_SET_UPVALUE: {
        uint8_t slot = READ_BYTE();
        ObjUpvalue* upvalue = vm.currentFrame->closure.mlip().mp()->upvalues.mlp().mp()[slot].mlip().mp();  //cb-resize-safe (no allocations in lifetime)
        if (upvalue->valueStackIndex == -1) {
          upvalue->closed = peek(0);
        } else {
          *tristack_at(&(vm.tristack), upvalue->valueStackIndex) = peek(0);
        }
        break;
      }

      case OP_GET_PROPERTY: {
        if (!IS_INSTANCE(peek(0))) {
          runtimeError("Only instances have properties.");
          return INTERPRET_RUNTIME_ERROR;
        }

        OID<ObjInstance> instance = AS_INSTANCE_OID(peek(0));
        Value name = READ_CONSTANT();
        Value value;
        assert(IS_STRING(name));
        if (instanceFieldGet(instance, name, &value)) {
          pop(); // Instance.
          push(value);
          break;
        }

        if (!bindMethod(instance.clip().cp()->klass, name)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }

      case OP_SET_PROPERTY: {
        if (!IS_INSTANCE(peek(1))) {
          runtimeError("Only instances have fields.");
          return INTERPRET_RUNTIME_ERROR;
        }

        OID<ObjInstance> instance = AS_INSTANCE_OID(peek(1));
        Value name = READ_CONSTANT();
        assert(IS_STRING(name));
        instanceFieldSet(instance, name, peek(0));
        Value value = pop();
        pop();
        push(value);
        break;
      }

      case OP_GET_SUPER: {
        Value name = READ_CONSTANT();
        assert(IS_STRING(name));
        OID<ObjClass> superclass = AS_CLASS_OID(pop());
        if (!bindMethod(superclass, name)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }

      case OP_EQUAL: {
        Value b = pop();
        Value a = pop();
        push(BOOL_VAL(valuesEqual(a, b)));
        break;
      }

      case OP_GREATER:  BINARY_OP(BOOL_VAL, >); break;
      case OP_LESS:     BINARY_OP(BOOL_VAL, <); break;

      case OP_ADD: {
        if (IS_STRING(peek(0)) && IS_STRING(peek(1))) {
          concatenate();
        } else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
          double b = AS_NUMBER(pop());
          double a = AS_NUMBER(pop());
          push(NUMBER_VAL(a + b));
        } else {
          runtimeError("Operands must be two numbers or two strings.");
          return INTERPRET_RUNTIME_ERROR;
        }
        break;
      }

      case OP_SUBTRACT: BINARY_OP(NUMBER_VAL, -); break;
      case OP_MULTIPLY: BINARY_OP(NUMBER_VAL, *); break;
      case OP_DIVIDE:   BINARY_OP(NUMBER_VAL, /); break;

      case OP_NOT:
        push(BOOL_VAL(isFalsey(pop())));
        break;

      case OP_NEGATE:
        if (!IS_NUMBER(peek(0))) {
          runtimeError("Operand must be a number.");
          return INTERPRET_RUNTIME_ERROR;
        }

        push(NUMBER_VAL(-AS_NUMBER(pop())));
        break;

      case OP_PRINT:
        printValue(pop(), true);
        printf("\n");
        break;

      case OP_JUMP: {
        uint16_t offset = READ_SHORT();
        vm.currentFrame->ip += offset;
        break;
      }

      case OP_JUMP_IF_FALSE: {
        uint16_t offset = READ_SHORT();
        if (isFalsey(peek(0))) vm.currentFrame->ip += offset;
        break;
      }

      case OP_LOOP: {
        integrate_any_gc_response();

        uint16_t offset = READ_SHORT();
        vm.currentFrame->ip -= offset;
        break;
      }

      case OP_CALL: {
        integrate_any_gc_response();
        int argCount = READ_BYTE();
        if (!callValue(peek(argCount), argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        vm.currentFrame = triframes_currentFrame(&(vm.triframes));
        break;
      }

      case OP_INVOKE: {
        integrate_any_gc_response();
        Value method = READ_CONSTANT();
        int argCount = READ_BYTE();
        assert(IS_STRING(method));
        if (!invoke(method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        vm.currentFrame = triframes_currentFrame(&(vm.triframes));
        break;
      }

      case OP_SUPER_INVOKE: {
        integrate_any_gc_response();
        Value method = READ_CONSTANT();
        assert(IS_STRING(method));
        int argCount = READ_BYTE();
        OID<ObjClass> superclass = AS_CLASS_OID(pop());
        if (!invokeFromClass(superclass, method, argCount)) {
          return INTERPRET_RUNTIME_ERROR;
        }
        vm.currentFrame = triframes_currentFrame(&(vm.triframes));
        break;
      }

      case OP_CLOSURE: {
        perform_OP_CLOSURE();
        break;
      }

      case OP_CLOSE_UPVALUE: {
        closeUpvalues(vm.tristack.stackDepth - 1);
        pop();
        break;
      }

      case OP_RETURN: {
        integrate_any_gc_response();

        Value result = pop();
        unsigned int oldFrameSlotsIndex = vm.currentFrame->slotsIndex;

        // Close any upvalues still in scope.
        closeUpvalues(vm.currentFrame->slotsIndex);

        // The frame we're leaving may not even be in the mutable A region if
        // we have recently performed a GC, so we cannot do the following:
        //assert(frame->slotsIndex >= vm.tristack.abi);

        triframes_leaveFrame(&(vm.triframes));  // NOTE: does not yet update our local variable 'frame'.
        if (triframes_frameCount(&(vm.triframes)) == 0) {
          pop();
          return INTERPRET_OK;
        }

        //NOTE: The purpose of this section is to move "up" (read: lower in
        // index) the stack. If we've returned to a frame whose slotsIndex is at
        // a lower index than where abi begins (such that it is not in the
        // mutable A region), we must move the contents of this returned-to
        // frame into the mutable section A and shift abi to reflect that this
        // portion of the stack indexes is now mutable.  This will maintain the
        // invariant that the present frame's portion of the stack is always
        // contiguous in the mutable section A.
        vm.currentFrame = triframes_currentFrame(&(vm.triframes));
        if (vm.currentFrame->slotsIndex < vm.tristack.abi) {
          vm.tristack.abi = vm.currentFrame->slotsIndex;
          memcpy(tristack_at(&(vm.tristack), vm.tristack.abi),
                 tristack_at_bc(&(vm.tristack), vm.currentFrame->slotsIndex),
                 (oldFrameSlotsIndex - vm.currentFrame->slotsIndex) * sizeof(Value));
          vm.currentFrame->slots = tristack_at(&(vm.tristack), vm.currentFrame->slotsIndex);  //re-derive pointer
          //FIXME update adirect
        }

        // Shorten the stack to whatever was its depth prior to entering the
        // frame we have now returned from.
        vm.tristack.stackDepth = oldFrameSlotsIndex;
        assert(vm.currentFrame->slots == tristack_at(&(vm.tristack), vm.currentFrame->slotsIndex));
        assert(vm.currentFrame->slots >= cb_at(thread_cb, vm.tristack.abo));  //Slots must be contiguous, and in mutable section A.
        assert(vm.currentFrame->slotsIndex >= vm.tristack.abi);

        //Place the return value into the value stack.
        push(result);

        break;
      }

      case OP_CLASS: {
        OID<ObjClass> klass = newClass(AS_STRING_OID(READ_CONSTANT()));
        push(OBJ_VAL(klass.id()));
        break;
      }

      case OP_INHERIT: {
        Value superclass = peek(1);
        if (!IS_CLASS(superclass)) {
          runtimeError("Superclass must be a class.");
          return INTERPRET_RUNTIME_ERROR;
        }

        classMethodsAddAll(AS_CLASS_OID(peek(0)), AS_CLASS_OID(superclass));
        pop(); // Subclass.
        break;
      }

      case OP_METHOD:
        Value name = READ_CONSTANT();
        assert(IS_STRING(name));
        defineMethod(name);
        break;
    }

#if KLOX_ILAT
    ticks t1 = getticks();

    lats[instruction].count++;
    lats[instruction].total_lat += (t1 - t0);
#endif //KLOX_ILAT

  }
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef BINARY_OP
}

InterpretResult interpret(const char* source) {
  {
    PIN_SCOPE;

    exec_phase = EXEC_PHASE_COMPILE;
    OID<ObjFunction> function = compile(source);
    if (function.is_nil()) return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function.id()));
    OID<ObjClosure> closure = newClosure(function);
    pop();

    //BUGFIX: without this, upstream code derived frames[0]->slots outside of stack.
    //  This was harmless, but trips our careful assertions.
    push(OBJ_VAL(closure.id()));

    callValue(OBJ_VAL(closure.id()), 0);
  }

  exec_phase = EXEC_PHASE_INTERPRET;
  InterpretResult result = run();

#if KLOX_ILAT
  uint64_t total_lat = 0;
  for (int i = 0; i < OP_METHOD+1; ++i) { total_lat += lats[i].total_lat; }

  FILE *ilatf = fopen("ilat.out", "a");
  fprintf(ilatf, "#\n");

#define PRINTIT(x) do { \
    if (lats[x].count > 0) { \
      fprintf(ilatf, "%-16s   count: %10ju   avgcost: % 9.1f  total_lat: %10ju  pct_total_lat: % 2.1f%%\n", \
          #x, \
          (uintmax_t)lats[x].count, \
          (double)lats[x].total_lat / (double)lats[x].count, \
          (uintmax_t)lats[x].total_lat, \
          (double)lats[x].total_lat / (double)total_lat * 100.0f \
          ); \
    } } while(0)

  PRINTIT(OP_CONSTANT);
  PRINTIT(OP_NIL);
  PRINTIT(OP_TRUE);
  PRINTIT(OP_FALSE);
  PRINTIT(OP_POP);
  PRINTIT(OP_GET_LOCAL);
  PRINTIT(OP_SET_LOCAL);
  PRINTIT(OP_GET_GLOBAL);
  PRINTIT(OP_DEFINE_GLOBAL);
  PRINTIT(OP_SET_GLOBAL);
  PRINTIT(OP_GET_UPVALUE);
  PRINTIT(OP_SET_UPVALUE);
  PRINTIT(OP_GET_PROPERTY);
  PRINTIT(OP_SET_PROPERTY);
  PRINTIT(OP_GET_SUPER);
  PRINTIT(OP_EQUAL);
  PRINTIT(OP_GREATER);
  PRINTIT(OP_LESS);
  PRINTIT(OP_ADD);
  PRINTIT(OP_SUBTRACT);
  PRINTIT(OP_MULTIPLY);
  PRINTIT(OP_DIVIDE);
  PRINTIT(OP_NOT);
  PRINTIT(OP_NEGATE);
  PRINTIT(OP_PRINT);
  PRINTIT(OP_JUMP);
  PRINTIT(OP_JUMP_IF_FALSE);
  PRINTIT(OP_LOOP);
  PRINTIT(OP_CALL);
  PRINTIT(OP_INVOKE);
  PRINTIT(OP_SUPER_INVOKE);
  PRINTIT(OP_CLOSURE);
  PRINTIT(OP_CLOSE_UPVALUE);
  PRINTIT(OP_RETURN);
  PRINTIT(OP_CLASS);
  PRINTIT(OP_INHERIT);
  PRINTIT(OP_METHOD);

#undef PRINTIT
  fclose(ilatf);

#endif //KLOX_ILAT

  return result;
}
