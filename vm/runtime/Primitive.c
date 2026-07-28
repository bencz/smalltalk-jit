#include "runtime/primitives/Shared.h"
#include "core/Assert.h"
#include <string.h>

// THE TABLE, and nothing else.
//
// The implementations live one file per domain in runtime/primitives/, and the
// domains are the ones this file was already sectioned into before it was split:
// arithmetic, comparison, bits, identity, allocation, blocks, float mathematics,
// time, strings, exceptions, streams, characters, printing and indexed access.
//
// What stays here is the mapping from a primitive number to one of them, plus
// the four functions that read it. It is deliberately the only thing here: the
// table is generated from runtime/Primitives.def, so a file that also held
// implementations would be a file whose contents were half generated and half
// not, and the generated half is the part that must not acquire exceptions.
//
// Why the implementations are extern rather than static-and-#included, and why
// that decision is what shaped Primitives.def, is written in
// runtime/primitives/Shared.h. In short: nothing else in this tree #includes a
// .c, and hand-writing the declarations would have been a second copy of a list
// that already exists.
//
// EVERY ONE of them obeys two rules, and both are load-bearing rather than
// stylistic:
//
//   1. NO ALLOCATION, except in the ones that exist to allocate, and those
//      ANCHOR THE CALLING FRAME first (PRIMITIVE_ALLOCATES). Allocating without
//      anchoring means a collection triggered from inside a primitive walks no
//      compiled frames, and evacuates objects the live method underneath is
//      still holding in its registers. Everything else answers an immediate or
//      an object the caller already holds.
//   2. FAIL RATHER THAN GUESS. A primitive that cannot answer exactly returns
//      PRIMITIVE_FAILED and lets the method's bytecode handle the general case.
//      Overflow fails. A zero divisor fails. An out-of-range index fails. A
//      receiver of the wrong shape fails. None of them approximate.

// ---------------------------------------------------------------------------
// Weak defaults for the primitives that need the FRONT END
// ---------------------------------------------------------------------------
//
// The table names every implemented primitive, so every link that includes this
// file must resolve every one of them. That is fine for domains that need only
// the object model -- but the reflective compiler (runtime/primitives/Reflect.c)
// needs the parser, the compiler and the class builder, and gate levels 3 and 7
// exist precisely to prove the JIT and the primitives with NO front end
// underneath (docs/jit-v2/01-gate.md). Linking the compiler in to satisfy the
// table would dissolve the property those levels are for.
//
// So they are WEAK here and STRONG in Reflect.c. This is the same seam
// memory/Roots.c already uses, and for the same reason: a lower layer has to be
// able to link without a higher one. These are DEFAULTS, not stubs. When
// Reflect.c is linked the strong definitions win; when it is not, the primitive
// answers PRIMITIVE_FAILED, which is exactly the "declared but not implemented"
// state every absent primitive is in, and the method runs its Smalltalk
// fallback.
//
// A front-end-dependent primitive added WITHOUT a default here is a link error
// in levels 3 and 7, which is the loud failure and the intended one.
#define WEAK_FRONTEND_PRIMITIVE(name) \
	__attribute__((weak)) Value name(Value *args, uint64_t argc) \
	{ \
		(void) args; \
		(void) argc; \
		return PRIMITIVE_FAILED; \
	}

WEAK_FRONTEND_PRIMITIVE(primParseClass)
WEAK_FRONTEND_PRIMITIVE(primParseMethod)
WEAK_FRONTEND_PRIMITIVE(primParseMethodOrBlock)
WEAK_FRONTEND_PRIMITIVE(primBuildClass)
WEAK_FRONTEND_PRIMITIVE(primCompileMethod)


// Designated initialisers, so the array is indexed by the enum and a primitive
// added out of order still lands in its own slot. The old VM's table was
// positional and its comment said in capitals never to reorder it.
//
// PRIMITIVE_ABSENT is where the NULL comes from now. It is the one place in the
// system that turns "the kernel names this and we have not written it" into a
// null function pointer, and primitiveFunctionAt's contract is built on that.
static const struct {
	PrimitiveFunction function;
	const char *name;
} gPrimitives[PRIM_COUNT] = {
	[PRIM_NONE] = { NULL, "none" },
#define PRIMITIVE(id, name, function) [PRIM_##id] = { function, name },
#define PRIMITIVE_ABSENT(id, name) [PRIM_##id] = { NULL, name },
#include "runtime/Primitives.def"
#undef PRIMITIVE
#undef PRIMITIVE_ABSENT
};


PrimitiveFunction primitiveFunctionAt(PrimitiveNumber number)
{
	// An out-of-range number is a bug in whoever built the method, and it must
	// not degrade into a send that quietly answers nothing.
	ASSERT(number < PRIM_COUNT);
	// NULL is legal and means DECLARED BUT NOT IMPLEMENTED: the method compiles
	// and runs its Smalltalk fallback. The caller checks; nothing here guesses.
	return gPrimitives[number].function;
}


const char *primitiveName(PrimitiveNumber number)
{
	ASSERT(number < PRIM_COUNT);
	return gPrimitives[number].name;
}


PrimitiveNumber primitiveNumberNamed(const char *name, size_t length)
{
	// Linear over 175 entries, once per method COMPILED, which is not a path
	// worth a hash table until it shows up in a profile.
	for (int i = 1; i < PRIM_COUNT; i++) {
		const char *candidate = gPrimitives[i].name;
		if (strncmp(candidate, name, length) == 0 && candidate[length] == 0) {
			return (PrimitiveNumber) i;
		}
	}
	return PRIM_NONE;
}


void primitiveCoverage(size_t *implemented, size_t *declared)
{
	size_t have = 0;
	for (int i = 1; i < PRIM_COUNT; i++) {
		if (gPrimitives[i].function != NULL) {
			have++;
		}
	}
	if (implemented != NULL) {
		*implemented = have;
	}
	if (declared != NULL) {
		*declared = (size_t) PRIM_COUNT - 1;
	}
}
