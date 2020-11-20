#ifndef klox_compiler_h
#define klox_compiler_h

#include "cb_integration.h"
#include "object.h"
#include "vm.h"

OID<ObjFunction> compile(const char* source);
void grayCompilerRoots();

#endif
