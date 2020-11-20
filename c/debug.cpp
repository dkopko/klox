#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "object.h"
#include "trace.h"
#include "value.h"

void disassembleChunk(Chunk* chunk, const char* name) {
  KLOX_TRACE("BEGIN disassembleChunk() == %s ==\n", name);
  for (int offset = 0; offset < chunk->count;) {
    offset = disassembleInstruction(chunk, offset);
  }
  KLOX_TRACE("END disassembleChunk() == %s ==\n", name);
}

static int constantInstruction(const char* name, const Chunk* chunk,
                               int offset) {
  uint8_t constant = chunk->code.clp().cp()[offset + 1];
  (void)constant;
  KLOX_TRACE_("%-16s %4d '", name, constant);
  KLOX_TRACE_ONLY(printValue(chunk->constants.values.clp().cp()[constant], false));
  KLOX_TRACE_("'\n");
  return offset + 2;
}

static int invokeInstruction(const char* name, const Chunk* chunk,
                                int offset) {
  uint8_t constant = chunk->code.clp().cp()[offset + 1];
  uint8_t argCount = chunk->code.clp().cp()[offset + 2];
  (void)constant, (void)argCount;
  KLOX_TRACE_("%-16s (%d args) %4d '", name, argCount, constant);
  KLOX_TRACE_ONLY(printValue(chunk->constants.values.clp().cp()[constant], false));
  KLOX_TRACE_("'\n");
  return offset + 3;
}

static int simpleInstruction(const char* name, int offset) {
  KLOX_TRACE_("%s\n", name);
  return offset + 1;
}

static int byteInstruction(const char* name, const Chunk* chunk, int offset) {
  uint8_t slot = chunk->code.clp().cp()[offset + 1];
  (void)slot;
  KLOX_TRACE_("%-16s %4d\n", name, slot);
  return offset + 2;
}

static int jumpInstruction(const char* name, int sign, const Chunk* chunk,
                           int offset) {
  uint16_t jump = (uint16_t)(chunk->code.clp().cp()[offset + 1] << 8);
  jump |= chunk->code.clp().cp()[offset + 2];
  (void)jump;
  KLOX_TRACE_("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
  return offset + 3;
}

int disassembleInstruction(const Chunk* chunk, int offset) {
  KLOX_TRACE_("TRACE %04d ", offset);

#if 0
    KLOX_TRACE("DANDEBUG: chunk: %p\n", chunk);
    KLOX_TRACE("DANDEBUG: lines: %ju\n", (uintmax_t)chunk->lines.co());
    KLOX_TRACE("DANDEBUG: clp().cp(): %p\n", chunk->lines.clp().cp());
    KLOX_TRACE("DANDEBUG: offset: %d\n", offset);
    KLOX_TRACE("DANDEBUG: [offset]: %d\n", chunk->lines.clp().cp()[offset]);
    KLOX_TRACE("DANDEBUG: [offset-1]: %d\n", chunk->lines.clp().cp()[offset-1]);
#endif

  if (offset > 0 && chunk->lines.clp().cp()[offset] == chunk->lines.clp().cp()[offset - 1]) {
    KLOX_TRACE_("   | ");
  } else {
    KLOX_TRACE_("%4d ", chunk->lines.clp().cp()[offset]);
  }

  uint8_t instruction = chunk->code.clp().cp()[offset];
  switch (instruction) {
    case OP_CONSTANT:
      return constantInstruction("OP_CONSTANT", chunk, offset);
    case OP_NIL:
      return simpleInstruction("OP_NIL", offset);
    case OP_TRUE:
      return simpleInstruction("OP_TRUE", offset);
    case OP_FALSE:
      return simpleInstruction("OP_FALSE", offset);
    case OP_POP:
      return simpleInstruction("OP_POP", offset);
    case OP_GET_LOCAL:
      return byteInstruction("OP_GET_LOCAL", chunk, offset);
    case OP_SET_LOCAL:
      return byteInstruction("OP_SET_LOCAL", chunk, offset);
    case OP_GET_GLOBAL:
      return constantInstruction("OP_GET_GLOBAL", chunk, offset);
    case OP_DEFINE_GLOBAL:
      return constantInstruction("OP_DEFINE_GLOBAL", chunk, offset);
    case OP_SET_GLOBAL:
      return constantInstruction("OP_SET_GLOBAL", chunk, offset);
    case OP_GET_UPVALUE:
      return byteInstruction("OP_GET_UPVALUE", chunk, offset);
    case OP_SET_UPVALUE:
      return byteInstruction("OP_SET_UPVALUE", chunk, offset);
    case OP_GET_PROPERTY:
      return constantInstruction("OP_GET_PROPERTY", chunk, offset);
    case OP_SET_PROPERTY:
      return constantInstruction("OP_SET_PROPERTY", chunk, offset);
    case OP_GET_SUPER:
      return constantInstruction("OP_GET_SUPER", chunk, offset);
    case OP_EQUAL:
      return simpleInstruction("OP_EQUAL", offset);
    case OP_GREATER:
      return simpleInstruction("OP_GREATER", offset);
    case OP_LESS:
      return simpleInstruction("OP_LESS", offset);
    case OP_ADD:
      return simpleInstruction("OP_ADD", offset);
    case OP_SUBTRACT:
      return simpleInstruction("OP_SUBTRACT", offset);
    case OP_MULTIPLY:
      return simpleInstruction("OP_MULTIPLY", offset);
    case OP_DIVIDE:
      return simpleInstruction("OP_DIVIDE", offset);
    case OP_NOT:
      return simpleInstruction("OP_NOT", offset);
    case OP_NEGATE:
      return simpleInstruction("OP_NEGATE", offset);
    case OP_PRINT:
      return simpleInstruction("OP_PRINT", offset);
    case OP_JUMP:
      return jumpInstruction("OP_JUMP", 1, chunk, offset);
    case OP_JUMP_IF_FALSE:
      return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_LOOP:
      return jumpInstruction("OP_LOOP", -1, chunk, offset);
    case OP_CALL:
      return byteInstruction("OP_CALL", chunk, offset);
    case OP_INVOKE:
      return invokeInstruction("OP_INVOKE", chunk, offset);
    case OP_SUPER_INVOKE:
      return invokeInstruction("OP_SUPER_INVOKE", chunk, offset);
    case OP_CLOSURE: {
      offset++;
      uint8_t constant = chunk->code.clp().cp()[offset++];
      KLOX_TRACE_("%-16s %4d ", "OP_CLOSURE", constant);
      KLOX_TRACE_ONLY(printValue(chunk->constants.values.clp().cp()[constant], false));
      KLOX_TRACE_("\n");

      OID<ObjFunction> function = AS_FUNCTION_OID(
          chunk->constants.values.clp().cp()[constant]);
      for (int j = 0, e = function.clip().cp()->upvalueCount; j < e; j++) {
        int isLocal = chunk->code.clp().cp()[offset++];
        int index = chunk->code.clp().cp()[offset++];
        (void)isLocal, (void)index;
        KLOX_TRACE_("TRACE %04d      |                     %s %d\n",
               offset - 2, isLocal ? "local" : "upvalue", index);
      }

      return offset;
    }

    case OP_CLOSE_UPVALUE:
      return simpleInstruction("OP_CLOSE_UPVALUE", offset);
    case OP_RETURN:
      return simpleInstruction("OP_RETURN", offset);
    case OP_CLASS:
      return constantInstruction("OP_CLASS", chunk, offset);
    case OP_INHERIT:
      return simpleInstruction("OP_INHERIT", offset);
    case OP_METHOD:
      return constantInstruction("OP_METHOD", chunk, offset);
    default:
      KLOX_TRACE("Unknown opcode %d\n", instruction);
      return offset + 1;
  }
}
