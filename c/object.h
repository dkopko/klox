#ifndef klox_object_h
#define klox_object_h

#include "cb_integration.h"
#include "common.h"
#include "chunk.h"
#include "table.h"
#include "value.h"

#define OBJ_TYPE(value)         (AS_OBJ(value)->type)

#define IS_BOUND_METHOD(value)  isObjType(value, OBJ_BOUND_METHOD)
#define IS_CLASS(value)         isObjType(value, OBJ_CLASS)
#define IS_CLOSURE(value)       isObjType(value, OBJ_CLOSURE)
#define IS_FUNCTION(value)      isObjType(value, OBJ_FUNCTION)
#define IS_INSTANCE(value)      isObjType(value, OBJ_INSTANCE)
#define IS_NATIVE(value)        isObjType(value, OBJ_NATIVE)
#define IS_STRING(value)        isObjType(value, OBJ_STRING)

#define AS_BOUND_METHOD_OID(value)  (OID<ObjBoundMethod>(AS_OBJ_ID(value)))
#define AS_CLASS_OID(value)         (OID<ObjClass>(AS_OBJ_ID(value)))
#define AS_CLOSURE_OID(value)       (OID<ObjClosure>(AS_OBJ_ID(value)))
#define AS_FUNCTION_OID(value)      (OID<ObjFunction>(AS_OBJ_ID(value)))
#define AS_INSTANCE_OID(value)      (OID<ObjInstance>(AS_OBJ_ID(value)))
#define AS_UPVALUE_OID(value)      (OID<ObjUpvalue>(AS_OBJ_ID(value)))
#define AS_NATIVE(value)        ((OID<ObjNative>(AS_OBJ_ID(value)).clip().cp())->function)
#define AS_STRING_OID(value)        (OID<ObjString>(AS_OBJ_ID(value)))

typedef enum {
  OBJ_BOUND_METHOD,
  OBJ_CLASS,
  OBJ_CLOSURE,
  OBJ_FUNCTION,
  OBJ_INSTANCE,
  OBJ_NATIVE,
  OBJ_STRING,
  OBJ_UPVALUE
} ObjType;

struct sObj {
  ObjType type;
  OID<struct sObj> white_next;
};

typedef struct {
  Obj obj;
  int arity;
  int upvalueCount;
  Chunk chunk;
  OID<ObjString> name;
} ObjFunction;

typedef Value (*NativeFn)(int argCount, Value* args);

typedef struct {
  Obj obj;
  NativeFn function;
} ObjNative;

struct sObjString {
  Obj obj;
  int length;
  CBO<char> chars; // char[]
  uint32_t hash;
};

typedef struct sUpvalue {
  Obj obj;

  // Pointer to the variable this upvalue is referencing.
  int valueStackIndex;

  // If the upvalue is closed (i.e. the local variable it was pointing
  // to has been popped off the stack) then the closed-over value is
  // hoisted out of the stack into here. [value] is then be changed to
  // point to this.
  Value closed;

  // Open upvalues are stored in a linked list. This points (via an OID lookup
  // through the objtable) to the next one in that list.
  OID<struct sUpvalue> next;
} ObjUpvalue;

typedef struct {
  Obj obj;
  OID<ObjFunction> function;
  CBO<OID<ObjUpvalue> > upvalues;  //pointer to ObjUpvalue[] (used to be type ObjUpvalue**).
  int upvalueCount;
} ObjClosure;

typedef struct sObjClass {
  Obj obj;
  OID<ObjString> name;
  OID<struct sObjClass> superclass;  //struct sObjClass* (only pointer, not array).
  struct structmap methods_sm;
  struct structmap_entry _entries[(1 << METHODS_FIRSTLEVEL_BITS) - 1];
} ObjClass;

typedef struct {
  Obj obj;
  OID<ObjClass> klass;
  struct structmap fields_sm;
  struct structmap_entry _entries[(1 << FIELDS_FIRSTLEVEL_BITS) - 1];
} ObjInstance;

typedef struct {
  Obj obj;
  Value receiver;
  OID<ObjClosure> method;
} ObjBoundMethod;

OID<ObjBoundMethod> newBoundMethod(Value receiver, OID<ObjClosure> method);
OID<ObjClass> newClass(OID<ObjString> name);
OID<ObjClosure> newClosure(OID<ObjFunction> function);
OID<ObjFunction> newFunction();
OID<ObjInstance> newInstance(OID<ObjClass> klass);
OID<ObjNative> newNative(NativeFn function);
OID<ObjString> rawAllocateString(const char* chars, int length);
OID<ObjString> takeString(CBO<char> /*char[]*/ chars, int length);
OID<ObjString> copyString(const char* chars, int length);

OID<ObjUpvalue> newUpvalue(unsigned int valueStackIndex);
void printObject(ObjID id, cb_offset_t offset, const Obj *obj, bool pretty);
void printObjectValue(Value value, bool pretty);

static inline bool isObjType(Value value, ObjType type) {
  return IS_OBJ(value) && AS_OBJ(value)->type == type;
}

#endif
