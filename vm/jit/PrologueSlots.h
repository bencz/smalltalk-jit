#ifndef PROLOGUE_SLOTS_H
#define PROLOGUE_SLOTS_H

// Which frame slots generatePrologue must NOT bother nil-initialising, shared
// by BOTH backends.
//
// generatePrologue nils every local slot so that a temp written on only one arm
// of an inlined conditional never reads as stack garbage, and so the GC
// stackmap never marks a garbage slot as a live root. That is right for
// ordinary temps and wrong for two of them: generateContextDefinition runs
// IMMEDIATELY after the prologue and overwrites the return-IC slot, and in
// every case but one the context slot too. Between the nil store and the
// overwrite there is no call, no allocation, no safepoint and no Smalltalk
// code, so nothing can observe the nil -- it is a store whose only reader is
// the store that replaces it.
//
// This is two instructions off EVERY framed activation, which is the single
// most executed sequence the JIT emits: measured at 12.5% of Richards'
// retired instructions in method preamble, of which 18 instructions is the
// typical shape and 2 are these.
//
// The predicate lives here, in one copy, for the same reason SendClassify.h
// does: a backend that disagreed with the other about which slot is dead
// would not be slower, it would silently skip a nil the GC needs.

#include "jit/CodeGenerator.h"
#include "compiler/Compiler.h"
#include "core/Assert.h"

// Byte offset (from the frame pointer) of the return-IC / native-code slot.
// generateContextDefinition spills it unconditionally, in both backends, as
// its very first instruction.
static ptrdiff_t prologueDeadIcSlotOffset(void)
{
	return -(ptrdiff_t) sizeof(intptr_t);
}


// Byte offset of the context slot when its write is a plain store, or 0 (never
// a valid slot offset -- slots are negative) when it is not.
//
// The one case that must keep its nil is a framed METHOD with hasContext: there
// generateContextDefinition calls generateMethodContextAllocation, whose stub
// call is a GC point, and the context slot is written only AFTER it returns. A
// collection triggered by that allocation walks this frame with the slot still
// uninitialised. The block path (store the frame pointer into the context, then
// spillVar) and the hasContext == 0 method path (load the thread's dummy context
// from TLS, then store) are both plain stores with nothing in between.
static ptrdiff_t prologueDeadContextSlotOffset(CodeGenerator *generator)
{
	if (!generator->code.isBlock && generator->code.header.hasContext) {
		return 0;
	}
	return generator->regsAlloc.vars[CONTEXT_INDEX].frameOffset * (ptrdiff_t) sizeof(intptr_t);
}


// How many of the frameSize slots actually get a nil store, so a prologue whose
// every slot is dead can skip materialising nil as well.
static size_t prologueNilSlotCount(CodeGenerator *generator, size_t frameSize)
{
	ptrdiff_t deadIc = prologueDeadIcSlotOffset();
	ptrdiff_t deadContext = prologueDeadContextSlotOffset(generator);
	size_t count = 0;
	for (size_t i = 0; i < frameSize; i++) {
		ptrdiff_t offset = -(ptrdiff_t) (i + 1) * sizeof(intptr_t);
		if (offset != deadIc && offset != deadContext) {
			count++;
		}
	}
	return count;
}


// Called by each backend's generateContextDefinition for every frame slot it
// stores to.
static void prologueNoteContextDefStore(CodeGenerator *generator, ptrdiff_t offset)
{
	if (generator->contextDefWroteCount < 2) {
		generator->contextDefWrote[generator->contextDefWroteCount++] = offset;
	}
}


// The invariant that makes the skipping safe, checked at the end of
// generateContextDefinition: every slot the prologue declined to nil was in
// fact written here. Cheap, and it runs on every method the test suite
// compiles, which is the only place this class of bug is observable at all --
// a missing nil produces no test failure, because the scavenger silently
// rewrites an implausible stack slot to nil and the mutator then reads the
// value it expected. That is why this is an assertion and not a test.
static void prologueAssertDeadSlotsWritten(CodeGenerator *generator)
{
	ptrdiff_t dead[2] = { prologueDeadIcSlotOffset(), prologueDeadContextSlotOffset(generator) };
	for (size_t d = 0; d < 2; d++) {
		if (dead[d] == 0) {
			continue; // not skipped (0 is never a valid slot offset)
		}
		_Bool written = 0;
		for (uint8_t w = 0; w < generator->contextDefWroteCount; w++) {
			written = written || generator->contextDefWrote[w] == dead[d];
		}
		ASSERT(written);
	}
}

#endif
