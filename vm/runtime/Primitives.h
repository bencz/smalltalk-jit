#ifndef PRIMITIVES_H
#define PRIMITIVES_H

#include "core/Object.h"
#include "jit/CodeGenerator.h"

// The C-primitive ABI, shared by the arch-neutral dispatch (Primitives.c) and
// each CPU backend's generators (vm/jit/<arch>/Primitives<Arch>.c): a CCALL
// primitive returns a PrimitiveResult; failed=1 means "fall through to the
// Smalltalk fallback code after the <primitive:> pragma".
typedef struct {
	Value value;
	intptr_t failed;
} PrimitiveResult;

static inline PrimitiveResult primSuccess(Value resultValue)
{
	PrimitiveResult result = { .value = resultValue, .failed = 0 };
	return result;
}

static inline PrimitiveResult primFailed(void)
{
	PrimitiveResult result = { .failed = 1 };
	return result;
}

void registerPrimitives(void);
void generatePrimitive(CodeGenerator *generator, uint16_t primitive);
uint16_t primitiveCount(void);   // valid primitive numbers are 1..primitiveCount()

#endif
