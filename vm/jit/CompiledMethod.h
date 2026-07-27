#ifndef COMPILED_METHOD_H
#define COMPILED_METHOD_H

// A compiled method as a heap object, so it can live in a class's method
// dictionary and be found by an ordinary dictionary lookup.
//
// FORMAT_MIXED_BYTES: three raw words the collector must not follow, then the
// tagged fields. The raw words hold C pointers (the bytecode unit and its
// compiled machine code), and getting the format wrong here would have the
// collector chase a code address as if it were an object.

#include "compiler/Bytecode.h"
#include "core/Handle.h"
#include "core/Object.h"
#include "jit/Jit.h"
#include "runtime/String.h"

typedef struct {
	OBJECT_HEADER;
	uint64_t bytecodeBytes; // raw word 0
	CodeUnit *unit;         // raw word 1
	NativeCode *native;     // raw word 2, NULL until first call
	Value selector;         // pointer word 0
	Value ownerClass;       // pointer word 1
} RawCompiledMethod;
OBJECT_HANDLE(CompiledMethod);

#define COMPILED_METHOD_RAW_WORDS 3
#define COMPILED_METHOD_POINTER_WORDS 2
#define COMPILED_METHOD_SHAPE \
	((InstanceShape) DEFINE_SHAPE(FORMAT_MIXED_BYTES, COMPILED_METHOD_RAW_WORDS, \
		COMPILED_METHOD_POINTER_WORDS, 0))

CompiledMethod *compiledMethodCreate(CodeUnit *unit, String *selector, Class *owner);

#endif
