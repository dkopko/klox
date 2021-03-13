#include <stdio.h>
#include <string.h>

#include "cb_bst.h"

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#define ALLOCATE_OBJ(type, objectType) \
    allocateObject(sizeof(type), cb_alignof(type), objectType)

static const char *
objTypeString(ObjType objType)
{
  switch (objType)
  {
    case OBJ_BOUND_METHOD: return "ObjBoundMethod";
    case OBJ_CLASS:        return "ObjClass";
    case OBJ_CLOSURE:      return "ObjClosure";
    case OBJ_FUNCTION:     return "ObjFunction";
    case OBJ_INSTANCE:     return "ObjInstance";
    case OBJ_NATIVE:       return "ObjNative";
    case OBJ_STRING:       return "ObjString";
    case OBJ_UPVALUE:      return "ObjUpvalue";
    default:               return "Obj???";
  }
}

static cb_offset_t allocateObject(size_t size, size_t alignment, ObjType type) {
  CBO<Obj> objectCBO = reallocate(CB_NULL, 0, size, alignment, true, false);

  Obj* object = objectCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  object->type = type;

#ifdef DEBUG_TRACE_GC
  KLOX_TRACE("@%ju %s object allocated (%ld bytes)\n",
             (uintmax_t)objectCBO.co(),
             objTypeString(type),
             size);
#endif

  return objectCBO.co();
}

static ObjID assignObjectToID(cb_offset_t offset) {
  OID<Obj> objectOID = objtable_add(&thread_objtable, offset);

#ifdef DEBUG_TRACE_GC
  KLOX_TRACE("#%ju -> @%ju object assigned\n",
             (uintmax_t)objectOID.id().id,
             (uintmax_t)offset);
#endif

  return objectOID.id();
}

OID<ObjBoundMethod> newBoundMethod(Value receiver, OID<ObjClosure> method) {
  CBO<ObjBoundMethod> boundCBO = ALLOCATE_OBJ(ObjBoundMethod, OBJ_BOUND_METHOD);
  ObjBoundMethod* bound = boundCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  bound->receiver = receiver;
  bound->method = method;
  return assignObjectToID(boundCBO.co());
}

OID<ObjClass> newClass(OID<ObjString> name) {
  int ret;

  (void)ret;

  CBO<ObjClass> klassCBO = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
  ObjClass* klass = klassCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  klass->name = name;
  ret = methods_layer_init(&thread_cb, &thread_region, &(klass->methods_sm));
  assert(ret == 0);

  return assignObjectToID(klassCBO.co());
}

OID<ObjClosure> newClosure(OID<ObjFunction> function) {
  PIN_SCOPE;
  int upvalueCount = function.clip().cp()->upvalueCount;
  CBO<OID<ObjUpvalue> > upvaluesCBO = ALLOCATE(OID<ObjUpvalue>, upvalueCount);
  CBO<ObjClosure> closureCBO = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);

  OID<ObjUpvalue>* upvalues = upvaluesCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  for (int i = 0; i < upvalueCount; i++) {
    upvalues[i] = CB_NULL_OID;
  }

  ObjClosure* closure = closureCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  closure->function = function;
  closure->upvalues = upvaluesCBO;
  closure->upvalueCount = upvalueCount;
  return assignObjectToID(closureCBO.co());
}

OID<ObjFunction> newFunction() {
  CBO<ObjFunction> functionCBO = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);
  ObjFunction* function = functionCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)

  function->arity = 0;
  function->upvalueCount = 0;
  function->name = CB_NULL_OID;
  initChunk(functionCBO.co());
  return assignObjectToID(functionCBO.co());
}

OID<ObjInstance> newInstance(OID<ObjClass> klass) {
  CBO<ObjInstance> instanceCBO = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
  int ret;

  (void)ret;

  ObjInstance* instance = instanceCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  instance->klass = klass;
  ret = fields_layer_init(&thread_cb, &thread_region, &(instance->fields_sm));
  assert(ret == 0);

  return assignObjectToID(instanceCBO.co());
}

OID<ObjNative> newNative(NativeFn function) {
  PIN_SCOPE;
  CBO<ObjNative> nativeCBO = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
  ObjNative* native = nativeCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  native->function = function;
  return assignObjectToID(nativeCBO.co());
}

static OID<ObjString> allocateString(CBO<char> adoptedChars, int length,
                                     uint32_t hash) {
  PIN_SCOPE;
  CBO<ObjString> stringCBO = ALLOCATE_OBJ(ObjString, OBJ_STRING);
  ObjString* string = stringCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  string->length = length;
  string->chars = adoptedChars;
  string->hash = hash;

  OID<ObjString> stringOID = assignObjectToID(stringCBO.co());
  Value stringValue = OBJ_VAL(stringOID.id());
  push(stringValue);
  KLOX_TRACE("interned string#%ju@%ju\"%.*s\"@%ju\n",
             (uintmax_t)stringOID.id().id,
             (uintmax_t)stringCBO.co(),
             length,
             adoptedChars.clp().cp(),
             (uintmax_t)adoptedChars.co());
  tableSet(&vm.strings, stringValue, stringValue);
  pop();

  return stringOID;
}

static uint32_t hashString(const char* key, int length) {
  uint32_t hash = 2166136261u;

  for (int i = 0; i < length; i++) {
    hash ^= key[i];
    hash *= 16777619;
  }

  return hash;
}

//NOTE: 'length' does not include the null-terminator.
OID<ObjString> rawAllocateString(const char* chars, int length) {
  PIN_SCOPE;
  uint32_t hash = hashString(chars, length);

  CBO<char> heapCharsCBO = ALLOCATE(char, length + 1);
  CBO<ObjString> stringCBO = ALLOCATE_OBJ(ObjString, OBJ_STRING);

  char* heapChars = heapCharsCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  memcpy(heapChars, chars, length);
  heapChars[length] = '\0';

  ObjString* string = stringCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  string->length = length;
  string->chars = heapCharsCBO;
  string->hash = hash;

  OID<ObjString> stringOID = assignObjectToID(stringCBO.co());
  KLOX_TRACE("created new string#%ju@%ju\"%.*s\"@%ju\n",
             (uintmax_t)stringOID.id().id,
             (uintmax_t)stringCBO.co(),
             length,
             heapChars,
             (uintmax_t)heapCharsCBO.co());

  return stringOID;
}

OID<ObjString> takeString(CBO<char> /*char[]*/ adoptedChars, int length) {
  uint32_t hash = hashString(adoptedChars.clp().cp(), length);
  OID<ObjString> internedOID = tableFindString(&vm.strings, adoptedChars.co(), adoptedChars.clp().cp(), length,
                                                hash);
  if (!internedOID.is_nil()) {
    FREE_ARRAY(char, adoptedChars.co(), length + 1);
    KLOX_TRACE("interned rawchars@%ju\"%.*s\" to string#%ju@%ju\"%s\"%ju\n",
               (uintmax_t)adoptedChars.co(),
               length,
               adoptedChars.clp().cp(),
               (uintmax_t)internedOID.id().id,
               (uintmax_t)internedOID.co(),
               internedOID.clip().cp()->chars.clp().cp(),
               (uintmax_t)internedOID.clip().cp()->chars.co());
    return internedOID;
  }

  KLOX_TRACE("could not find interned string for rawchars@%ju\"%.*s\"\n",
             (uintmax_t)adoptedChars.co(),
             length,
             adoptedChars.clp().cp());

  return allocateString(adoptedChars, length, hash);
}

OID<ObjString> copyString(const char* chars, int length) {
  PIN_SCOPE;
  uint32_t hash = hashString(chars, length);
  OID<ObjString> internedOID = tableFindString(&vm.strings, CB_NULL, chars, length,
                                               hash);
  if (!internedOID.is_nil()) {
    KLOX_TRACE("interned C-string \"%.*s\" to string#%ju@%ju\"%s\"\n",
               length,
               chars,
               (uintmax_t)internedOID.id().id,
               (uintmax_t)internedOID.clip().cp()->chars.co(),
               internedOID.clip().cp()->chars.clp().cp());
    return internedOID;
  }

  KLOX_TRACE("could not find interned string for C-string \"%.*s\"\n",
             length, chars);

  CBO<char> heapCharsCBO = ALLOCATE(char, length + 1);
  char* heapChars = heapCharsCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  memcpy(heapChars, chars, length);
  heapChars[length] = '\0';

  return allocateString(heapCharsCBO, length, hash);
}

OID<ObjUpvalue> newUpvalue(unsigned int valueStackIndex) {
  CBO<ObjUpvalue> upvalueCBO = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
  ObjUpvalue* upvalue = upvalueCBO.mlp().mp();  //cb-resize-safe (no allocations in lifetime)
  upvalue->closed = NIL_VAL;
  upvalue->valueStackIndex = valueStackIndex;
  upvalue->next = CB_NULL_OID;

  return assignObjectToID(upvalueCBO.co());
}

static void printFunction(ObjFunction* function) {
  if (function->name.is_nil()) {
    printf("<script>");
    return;
  }
  printf("<fn %s>", function->name.clip().cp()->chars.clp().cp());
}

void printObject(ObjID id, cb_offset_t offset, const Obj *obj, bool pretty) {
  if (!can_print)
    return;

  if (!obj) {
    printf("nullobj#%ju@%ju", (uintmax_t)id.id, (uintmax_t)offset);
    return;
  }

  switch (obj->type) {
    case OBJ_CLASS:
      if (pretty) {
        printf("%s", ((const ObjClass *)obj)->name.clip().cp()->chars.clp().cp());
      } else {
        if (!((const ObjClass *)obj)->name.is_valid()) {
          printf("class#%ju@%ju,name:<STALE>",
                 (uintmax_t)id.id,
                 (uintmax_t)offset);
        } else {
          printf("class#%ju@%ju,name:\"%s\"",
                 (uintmax_t)id.id,
                 (uintmax_t)offset,
                 ((const ObjClass *)obj)->name.clip().cp()->chars.clp().cp());
        }
      }
      break;

    case OBJ_BOUND_METHOD:
      if (pretty) {
        printf("<fn method>");
      } else {
        if (!((const ObjBoundMethod *)obj)->method.is_valid() ||
            !((const ObjBoundMethod *)obj)->method.clip().cp()->function.is_valid() ||
            !((const ObjBoundMethod *)obj)->method.clip().cp()->function.clip().cp()->name.is_valid()) {
          printf("boundmethod#%ju@%ju,name:<STALE>",
                 (uintmax_t)id.id,
                 (uintmax_t)offset);
        } else {
          printf("boundmethod#%ju@%ju,name:\"%s\"",
                 (uintmax_t)id.id,
                 (uintmax_t)offset,
                 ((const ObjBoundMethod *)obj)->method.clip().cp()->function.clip().cp()->name.clip().cp()->chars.clp().cp());
        }
      }
      break;

    case OBJ_CLOSURE: {
      const ObjClosure *clo = (const ObjClosure *)obj;
      if (pretty) {
          printf("<fn %s>",
                 clo->function.clip().cp()->name.clip().cp()->chars.clp().cp());
      } else {
        if (!clo->function.is_valid()) {
            printf("closure#%ju@%ju(fun#%ju@%ju)",
                   (uintmax_t)id.id,
                   (uintmax_t)offset,
                   (uintmax_t)clo->function.id().id,
                   (uintmax_t)clo->function.co());
        } else if (clo->function.clip().cp()->name.is_nil()) {
            printf("closure#%ju@%ju(fun#%ju@%ju,name:<anon>)",
                   (uintmax_t)id.id,
                   (uintmax_t)offset,
                   (uintmax_t)clo->function.id().id,
                   (uintmax_t)clo->function.co());
        } else if (!clo->function.clip().cp()->name.is_valid()) {
            printf("closure#%ju@%ju(fun#%ju@%ju,name:<STALE>)",
                   (uintmax_t)id.id,
                   (uintmax_t)offset,
                   (uintmax_t)clo->function.id().id,
                   (uintmax_t)clo->function.co());
        } else {
            printf("closure#%ju@%ju(fun#%ju@%ju,name:\"%s\")",
                   (uintmax_t)id.id,
                   (uintmax_t)offset,
                   (uintmax_t)clo->function.id().id,
                   (uintmax_t)clo->function.co(),
                   clo->function.clip().cp()->name.clip().cp()->chars.clp().cp());
        }

        printf("{upvalues:");
        for (int i = 0; i < clo->upvalueCount; ++i) {
          printf("[%d]:#%ju", i, (uintmax_t)clo->upvalues.clp().cp()[i].id().id);
          if (i < clo->upvalueCount - 1) printf(",");
        }
        printf("}");
      }
      break;
    }

    case OBJ_FUNCTION: {
      const ObjFunction *fun = (const ObjFunction *)obj;
      if (fun->name.is_nil()) {
        printf("fun#%ju@%ju,name:<anon>",
               (uintmax_t)id.id,
               (uintmax_t)offset);
      } else if (!fun->name.is_valid()) {
        printf("fun#%ju@%ju,name:<STALE>",
               (uintmax_t)id.id,
               (uintmax_t)offset);
      } else {
        printf("fun#%ju@%ju,name:\"%s\"",
               (uintmax_t)id.id,
               (uintmax_t)offset,
               fun->name.clip().cp()->chars.clp().cp());
      }
      break;
    }

    case OBJ_INSTANCE:
      if (pretty) {
        printf("%s instance",
               ((const ObjInstance *)obj)->klass.clip().cp()->name.clip().cp()->chars.clp().cp());
      } else {
        if (!((const ObjInstance *)obj)->klass.is_valid() ||
            !((const ObjInstance *)obj)->klass.clip().cp()->name.is_valid()) {
          printf("instance#%ju@%ju,classname:<STALE>",
                 (uintmax_t)id.id,
                 (uintmax_t)offset);
        } else {
          printf("instance#%ju@%ju,classname:\"%s\"",
                 (uintmax_t)id.id,
                 (uintmax_t)offset,
                 ((const ObjInstance *)obj)->klass.clip().cp()->name.clip().cp()->chars.clp().cp());
        }
      }
      break;

    case OBJ_NATIVE:
      if (pretty) {
        printf("<native fn>");
      } else {
        printf("native#%ju@%ju:%p",
               (uintmax_t)id.id,
               (uintmax_t)offset,
               (void*)(uintptr_t)((const ObjNative*)obj)->function);
      }
      break;

    case OBJ_STRING:
      if (pretty) {
        printf("%s", ((const ObjString *)obj)->chars.clp().cp());
      } else {
        printf("string#%ju@%ju\"%s\"@%ju",
               (uintmax_t)id.id,
               (uintmax_t)offset,
               ((const ObjString *)obj)->chars.clp().cp(),
               (uintmax_t)((const ObjString *)obj)->chars.co());
      }
      break;

    case OBJ_UPVALUE: {
      const ObjUpvalue *upvalue = (const ObjUpvalue *)obj;
      printf("upvalue#%ju@%ju",
             (uintmax_t)id.id,
             (uintmax_t)offset);
      if (upvalue->valueStackIndex == -1) {
        printf(":");
        printValue(upvalue->closed, false);
      } else {
        printf("^%d", upvalue->valueStackIndex);
      }
      break;
    }

    default:
      printf("badobj'%c'#%ju@%ju", obj->type, (uintmax_t)id.id, (uintmax_t)offset);
      abort();
      break;
  }
}

void printObjectValue(Value value, bool pretty) {
  OID<Obj> tmp = AS_OBJ_ID(value);

  if (!tmp.is_valid()) {
    printf("unknown#%ju@%ju", (uintmax_t)tmp.id().id, (uintmax_t)tmp.co());
    return;
  }

  printObject(tmp.id(), tmp.co(), tmp.clip().cp(), pretty);
}
