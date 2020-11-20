#ifndef klox_debug_h
#define klox_debug_h

#include "chunk.h"

void disassembleChunk(Chunk* chunk, const char* name);
int disassembleInstruction(const Chunk* chunk, int offset);

#endif
