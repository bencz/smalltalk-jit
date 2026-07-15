#ifndef TARGET_PRIMITIVES_H
#define TARGET_PRIMITIVES_H

// The primitive-generator contract every CPU backend must fulfil (bound at
// link time — CMake's ST_ARCH decides which vm/jit/<arch>/Primitives<Arch>.c
// provides the definitions). The Primitives[] table in vm/runtime/Primitives.c
// consumes these; the list itself lives in jit/PrimitivesGen.def.

#include "jit/CodeGenerator.h"
#include "runtime/Primitives.h"

#define GEN_PRIMITIVE(name, fn) void fn(CodeGenerator *generator);
#include "jit/PrimitivesGen.def"
#undef GEN_PRIMITIVE

// Trampoline generator for CCALL primitives: emits the call into a C
// PrimitiveResult function plus the primFailed -> fall-through-to-Smalltalk
// protocol around it.
void generateCCallPrimitive(CodeGenerator *generator, PrimitiveResult (*cFunction)(), size_t argsSize);

#endif
