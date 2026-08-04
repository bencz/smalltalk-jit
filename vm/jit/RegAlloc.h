#ifndef REGALLOC_H
#define REGALLOC_H

// Linear scan with splitting, over the LIR, with the two banks allocated
// independently.
//
// WHAT COMES OUT. Every operand of every instruction names a PHYSICAL register,
// and the moves between registers and spill slots are ordinary LIR_MOVE,
// LIR_LOAD_SLOT and LIR_STORE_SLOT instructions inserted into the stream. The
// emitter therefore never asks "where does this value live": by the time it
// runs, the question has one answer per operand and it is written down. That is
// the reason the split intervals are resolved here rather than exposed.
//
// AND THE FRAME MAPS. A map is built at every instruction execution can leave
// from, out of the live intervals at that position. This is the payoff ADR 0007
// describes: the map comes from the same liveness the allocator already
// computed, so it is EXACT, and the over-approximation the old VM needed at
// every back edge has nowhere to come from. If anything here ever grows an
// equivalent of overapproxStackmap, the decision was named and not implemented.

#include "jit/Lir.h"

typedef struct {
	uint32_t intervals;
	uint32_t spilled;       // intervals that lost their register at least once
	uint32_t splits;
	uint32_t spillSlots;
	uint32_t resolutionMoves;
	uint32_t frameMaps;
	uint32_t deoptSites;
	// States that could not be fully described in runtime terms. NOT a warning:
	// each one is a point the optimized code cannot be left at.
	uint32_t deoptIncomplete;
	// Which callee-saved registers the allocation used, so the prologue knows
	// what it owes. A BITSET per bank, because the two number independently.
	uint32_t calleeSavedUsed[LIR_BANK_COUNT];
	// The allocation did not hold together and the LIR must NOT be emitted. Its
	// own field rather than a zeroed count: a count that doubles as a failure
	// flag is a caller away from being read as "nothing to do".
	_Bool failed;
} RegAllocStats;

RegAllocStats lirAllocateRegisters(LirFunction *function);

// Does the finished allocation actually hold together? Answers 0 and prints the
// first thing wrong.
//
// A VERIFIER AND NOT A TEST, which is the same reasoning this repository already
// applies to stackmaps: the failure mode of a register allocator is two live
// values in one register, and that produces a wrong answer somewhere else
// entirely rather than a red test. Checking the OUTPUT against its own
// invariants catches it at the compilation that caused it, on every method the
// system ever compiles, which no amount of end-to-end testing does.
//
// Three invariants, and each fails for a different reason:
//
//   * no two intervals that overlap share a physical register;
//   * every use of a virtual register is reached by a definition of the
//     location it was allocated to;
//   * the frame map at each safepoint describes exactly the live pointers.
_Bool lirVerifyAllocation(const LirFunction *function);

#endif
