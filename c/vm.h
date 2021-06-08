#ifndef klox_vm_h
#define klox_vm_h

#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX 64
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

typedef struct {
  OID<ObjClosure> closure;
  OID<ObjFunction> function;
  const ObjFunction *functionP;
  const Value *constantsValuesP;
  union {
    size_t         ip_offset;
    const uint8_t *ip;
  };
  const uint8_t *ip_root;
  Value* slots;
  unsigned int slotsIndex;  // the index within the stack where slots field points.
  unsigned int slotsCount;
  unsigned int gc_integration_epoch;
  DEBUG_ONLY(bool has_ip_offset);
} CallFrame;

typedef struct {
  Value        *adirect;  //Cached pointer to the array at abo.
  Value        *bdirect;  //Cached pointer to the array at bbo.
  Value        *cdirect;  //CAched pointer to the array at cbo.
  cb_offset_t   abo; // A base offset (mutable region)
  cb_offset_t   bbo; // B base offset
  cb_offset_t   cbo; // C base offset
  unsigned int  stackDepth;  // [0, stack_depth-1] are valid entries.
  unsigned int  abi; // A base index  (mutable region)
  unsigned int  bbi; // B base index
  unsigned int  cbi; // C base index (always 0, really)
} __attribute__ ((aligned (64))) TriStack;

void tristack_recache(TriStack *ts, struct cb *target_cb);
Value* tristack_at(TriStack *ts, unsigned int index);

extern inline Value
tristack_peek(TriStack *ts, unsigned int down) {
  assert(down < ts->stackDepth);

  unsigned int element_index = (ts->stackDepth - 1 - down);
  return *tristack_at(ts, element_index);
}

extern inline void
tristack_discardn(TriStack *ts, unsigned int n) {
  assert(n <= ts->stackDepth);

  ts->stackDepth -= n;

  //Adjust A region if we have popped down below it.
  if (ts->stackDepth < ts->abi)
    ts->abi = ts->stackDepth;
}

extern inline void
tristack_push(TriStack *ts, Value v) {
  //CBINT FIXME how to check that there is room to push?  (old code didn't care)
  // We could make all stack edges be followed by a page which faults and
  // causes a reallocation of the stack.
  assert(ts->adirect == static_cast<Value*>(cb_at(thread_cb, ts->abo)));
  ts->adirect[ts->stackDepth - ts->abi] = v;
  ++(ts->stackDepth);
}

extern inline Value
tristack_pop(TriStack *ts) {
  assert(ts->stackDepth > 0);

  Value v = tristack_peek(ts, 0);
  tristack_discardn(ts, 1);
  return v;
}

typedef struct {
  CallFrame    *adirect;  //Cached pointer to the array at abo.
  CallFrame    *bdirect;  //Cached pointer to the array at bbo.
  CallFrame    *cdirect;  //Cached pointer to the array at cbo.
  CallFrame    *currentFrame;
  unsigned int  frameCount;  // [0, frameCount-1] are valid entries.
  cb_offset_t   abo; // A base offset (mutable region)
  cb_offset_t   bbo; // B base offset
  cb_offset_t   cbo; // C base offset
  unsigned int  abi; // A base index  (mutable region)
  unsigned int  bbi; // B base index
  unsigned int  cbi; // C base index (always 0, really)
} __attribute__ ((aligned (64))) TriFrames;

void triframes_ensureCurrentFrameIsMutable(TriFrames *tf);
void triframes_recache(TriFrames *tf, struct cb *target_cb);
CallFrame* triframes_at(TriFrames *tf, unsigned int index);
CallFrame* triframes_at_alt(TriFrames *tf, unsigned int index, struct cb *target_cb);


typedef struct {
  TriStack tristack;
  CallFrame *currentFrame;
  TriFrames triframes;
  Table globals;
  Table strings;
  OID<ObjString> initString;
  OID<ObjUpvalue> openUpvalues;  //Head of singly-linked openUpvalue list, not an array.

  size_t bytesAllocated;
  size_t nextGC;
} VM;

typedef struct {
  int grayCount;
  int grayCapacity;
  CBO<OID<Obj> >  grayStack;
} GC;

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR
} InterpretResult;

extern VM vm;
extern GC gc;

void tristack_print(TriStack *ts);
void triframes_print(TriFrames *tf);

extern inline unsigned int
triframes_frameCount(TriFrames *tf) {
  return tf->frameCount;
}

extern inline CallFrame*
triframes_currentFrame(TriFrames *tf) {
  assert((tf->frameCount == 0 && tf->currentFrame == NULL) || tf->currentFrame == triframes_at(tf, triframes_frameCount(tf) - 1));
  return tf->currentFrame;
}

void printCallFrame(CallFrame *frame);

void initVM();
void freeVM();
InterpretResult interpret(const char* source);

extern inline void
push(Value value) {
  tristack_push(&(vm.tristack), value);
}

extern inline Value
pop() {
  return tristack_pop(&(vm.tristack));
}

#endif
