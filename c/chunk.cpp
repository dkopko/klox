#include <stdlib.h>

#include <cb_bst.h>

#include "chunk.h"
#include "memory.h"
#include "value.h"
#include "vm.h"

void initChunk(CBO<Obj> f) {
  ObjFunction *mfun = (ObjFunction *)f.mlp().mp();
  Chunk *mchunk = &(mfun->chunk);

  mchunk->count = 0;
  mchunk->capacity = 0;
  mchunk->code = CB_NULL;
  mchunk->lines = CB_NULL;
  mchunk->constants.values = CB_NULL;
  mchunk->constants.capacity = 0;
  mchunk->constants.count = 0;
}

void writeChunk(OID<Obj> f, uint8_t byte, int line) {
  PIN_SCOPE;
  RCBP<ObjFunction> cfun = f.clip();

  int newCapacity = cfun.cp()->chunk.capacity;
  CBO<uint8_t> newCode;
  CBO<int > newLines;
  bool hasNewArrays = false;

  if (cfun.cp()->chunk.capacity < cfun.cp()->chunk.count + 1) {
    int oldCapacity = cfun.cp()->chunk.capacity;
    newCapacity = GROW_CAPACITY(oldCapacity);
    newCode = GROW_ARRAY(cfun.cp()->chunk.code.co(), uint8_t,
        oldCapacity, newCapacity);
    newLines = GROW_ARRAY(cfun.cp()->chunk.lines.co(), int,
        oldCapacity, newCapacity);
    hasNewArrays = true;

    //NOTE: Because this chunk extension is done to an chunk already present
    // in the objtable (it is held by an ObjFunction, which is held in the
    // objtable), we must manually inform the objtable of this independent
    // mutation of external size.
    objtable_external_size_adjust_A(&thread_objtable,
                                    (newCapacity - oldCapacity) * (sizeof(uint8_t) + sizeof(int)));
  }

  ObjFunction *mfun = (ObjFunction *)f.mlip().mp();
  Chunk *mchunk = &(mfun->chunk);

  mchunk->capacity = newCapacity;
  if (hasNewArrays) {
    mchunk->code = newCode;
    mchunk->lines = newLines;
  }
  mchunk->code.mlp().mp()[mchunk->count] = byte;
  mchunk->lines.mlp().mp()[mchunk->count] = line;
  mchunk->count++;
}

int addConstant(OID<Obj> f, Value value) {
  PIN_SCOPE;
  const ObjFunction *cfun = (const ObjFunction *)f.clip().cp();
  const Chunk *cchunk = &(cfun->chunk);

  push(value);  //Protect value from GC

  int newCapacity = cchunk->constants.capacity;
  CBO<Value> newValues = cchunk->constants.values;
  bool hasNewArray = false;

  if (cchunk->constants.capacity < cchunk->constants.count + 1) {
    int oldCapacity = cchunk->constants.capacity;
    newCapacity = GROW_CAPACITY(oldCapacity);
    newValues = GROW_ARRAY(cchunk->constants.values.co(), Value,
                           oldCapacity, newCapacity);
    hasNewArray = true;

    //NOTE: Because this constants extension is done to an chunk already present
    // in the objtable (it is held by an ObjFunction, which is held in the
    // objtable), we must manually inform the objtable of this independent
    // mutation of external size.
    objtable_external_size_adjust_A(&thread_objtable,
                                    (newCapacity - oldCapacity) * sizeof(Value));
  }

  ObjFunction *mfun = (ObjFunction *)f.mlip().mp();
  Chunk *mchunk = &(mfun->chunk);

  mchunk->constants.capacity = newCapacity;
  if (hasNewArray)
    mchunk->constants.values = newValues;
  mchunk->constants.values.mlp().mp()[mchunk->constants.count] = value;
  mchunk->constants.count++;

  pop();

  return mchunk->constants.count - 1;
}

