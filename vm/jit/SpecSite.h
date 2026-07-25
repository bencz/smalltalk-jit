#ifndef SPEC_SITE_H
#define SPEC_SITE_H

#include <stdint.h>

// A SPECULATION SITE: a place where emitted code assumes a lookup result that a
// later class redefinition, extension or method removal can falsify.
//
// Every speculation in this VM keeps a full-fidelity fallback in the SAME code
// (a guard's not-taken edge, or a cold re-resolve thunk), so invalidating one
// never needs deoptimization or a frame-state descriptor: it only needs to
// force the fallback to be taken from now on. That is what "poisoning" means
// here, and it is why this record stores a patch offset rather than any kind of
// dependency description.
//
// SPEC_GUARD: patchOffset addresses the guard's CONDITIONAL branch. Poison
//   rewrites it as an UNCONDITIONAL branch to the same target, so the fallback
//   (the original send, kept byte-identical) runs from then on. Branching is
//   uniform across every receiver shape while the baked class is NOT: an
//   immediate-class guard is a bare tag test with no class immediate to spoil.
//   auxOffset is unused.
// SPEC_STATIC: a compile-time devirtualized send, which has no guard and no IC
//   cell at all. Its callee entry is baked outright, so a redefinition of the
//   resolved class leaves it calling the replaced method. The send is emitted
//   as an unconditional jump OVER an inline re-resolve thunk; patchOffset
//   addresses that jump and poison zeroes its displacement so control falls
//   into the thunk instead. auxOffset is unused. The thunk is inline rather
//   than out of line because it can allocate (method lookup may compile), so
//   it has to run inside the caller's frame where the call site's stackmap
//   describes the stack correctly. Steady-state cost is one predicted
//   unconditional jump.
//
// Both poisons are bounded writes performed with the world stopped, inside the
// bracket icInvalidateAllSends already establishes.
//
// This lives in its own header because the layout is shared by the assembler
// (which collects sites during emission) and CompiledCode.h (which owns the
// NativeCode tail they are copied into), and CompiledCode.h already reaches
// Assembler.h transitively through compiler/Bytecodes.h.
enum {
	SPEC_GUARD = 1,
	SPEC_STATIC = 2,
};

typedef struct {
	uint32_t patchOffset;
	uint32_t auxOffset;
	uint32_t kind;
} SpecSite;

#endif
