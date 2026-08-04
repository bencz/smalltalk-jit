#ifndef LOWER_H
#define LOWER_H

// Instruction selection: the optimized SSA IR becomes LIR over virtual
// registers, ready for the allocator.
//
// REFUSES BY NAME, which is the same discipline ssaBuild uses one layer up and
// for the same reason. The front half of tier 2 covers every method in the
// kernel today; the back half does not, and the honest way to say so is a list
// of which operations it cannot yet emit rather than an estimate of how much is
// left. A silent skip would be a method compiled wrong.

#include "jit/Ir.h"
#include "jit/Lir.h"

struct NativeCode;

// `tier1` supplies the INLINE CACHE CELLS, which tier 2 shares rather than
// duplicates: a cell accumulates the profile, and a second set of them would
// split the profile of every site in two and leave a deoptimization landing on
// caches that had gone cold. It is also where the selector for a site lives, so
// the send path needs nothing else passed to it.
//
// Answers NULL when some operation cannot be lowered, with `refusedOp` set to
// the IR op's name.
LirFunction *lirLower(IrFunction *ir, const Abi *abi, struct NativeCode *tier1,
	const char **refusedOp);

#endif
