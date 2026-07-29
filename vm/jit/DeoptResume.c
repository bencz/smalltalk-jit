// Leaving optimized code: rebuilding the tier-1 frame and continuing in it.
//
// ---------------------------------------------------------------------------
// THE SHAPE, AND WHY IT IS NOT THE OBVIOUS ONE
// ---------------------------------------------------------------------------
//
// The obvious design REPLACES the running frame: overwrite the tier-2 frame
// with a tier-1 frame at the same address and jump into the middle of tier-1's
// code. It is what large VMs do and it needs a hand-written trampoline that
// moves the stack pointer out from under itself, which is the single most
// delicate piece of assembly in a JIT.
//
// This one STACKS instead. The failing guard calls out, the runtime builds a
// FRESH tier-1 frame on top of the tier-2 one, and execution continues there
// from the bytecode index the guard named. Its epilogue unwinds the trampoline's
// frame and hands the answer back here, and the guard site returns it as the
// method's answer -- so the rest of the method runs in tier 1 and the tier-2
// code below the guard never executes.
//
// What it costs: one dead tier-2 frame's worth of stack, alive until the method
// returns. What it buys: the trampoline is an ordinary function that sets up a
// frame and jumps, rather than one that dismantles the frame it is standing on.
// There is no window in which the stack pointer is wrong.
//
// The tier-2 frame underneath stays SCANNABLE the whole time, which is not an
// accident: its frame map still describes it and its slots still hold what they
// held, so a collection during the resumed execution walks it correctly.
//
// NOT ONE REGISTER NAME IN THIS FILE. The jump itself is the only part that
// needs any, and it lives in vm/jit/x64/abi/sysv/ where ADR 0009 puts it.

#include "jit/Deopt.h"
#include "jit/Jit.h"
#include "jit/Lir.h"
#include "core/Assert.h"
#include "core/Object.h"
#include "core/Handle.h"
#include "jit/SsaRuntime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Read one described value out of the optimized frame.
//
// A value that lived in a REGISTER is read from where the guard sequence
// spilled it, not from the register itself: by the time this C function runs,
// every register belongs to this function.
static Value readSlot(const DeoptSlot *slot, const uint8_t *frame,
	const Value *saved)
{
	Value raw;
	switch ((DeoptWhere) slot->where) {
	case DEOPT_CONSTANT:
		return slot->constant;
	case DEOPT_IN_REGISTER:
		// The save area descends like every other run of slots, so register r
		// is at saveBase + r, which is a LOWER address than saveBase.
		raw = saved[-slot->location];
		break;
	default:
		// Slot i sits at frame - 8*(i+1), the one law both tiers share.
		raw = ((const Value *) frame)[-(slot->location + 1)];
		break;
	}

	// RE-BOXED ON THE WAY OUT. Tier 1 has no notion of a raw double or a raw
	// integer in a frame slot -- that is the whole reason its frame map is one
	// line -- so a value the optimizer had unboxed has to become a Value again
	// before it can be handed over. Skipping this is a wrong answer that looks
	// like a huge integer.
	switch ((SlotKind) slot->kind) {
	case SLOT_F64:
		return jitBoxFloat(NULL, &raw, 0);
	case SLOT_I64:
		return tagInt((intptr_t) raw);
	default:
		return raw;
	}
}


// A guard failed. Rebuild and continue.
//
// ONE FRAME ONLY for now, and it is checked rather than assumed: outer frames
// appear when inlining does, and an inlined state resumed as if it were a
// single frame would continue in the wrong method entirely. The ASSERT is what
// makes that a stop rather than a wrong answer.
Value jitDeoptimize(void *sitePointer, Value *slotZero, uint64_t packed)
{
	(void) packed;
	const DeoptSite *site = sitePointer;
	ASSERT(site != NULL && site->frameCount == 1);
	const DeoptRuntimeFrame *frame = &site->frames[0];
	ASSERT(frame->innermost);

	// slotZero is the address of frame slot 0, and slot 0 is at frame - 8.
	const uint8_t *optimized = (const uint8_t *) slotZero + sizeof(Value);
	const Value *saved = (const Value *) optimized - (frame->saveBase + 1);

	CodeUnit *unit = frame->unit;
	NativeCode *baseline = frame->baseline;
	ASSERT(baseline != NULL);
	ASSERT(frame->bci < unit->instructionCount);

	// Every slot starts as nil, for the reason tier 1's prologue does it: a slot
	// the frame map calls a pointer and nobody wrote would hand the collector
	// stack garbage as a root. The state describes only the LIVE registers, so
	// the rest have to be filled here.
	//
	// ON THE HEAP and not an array sized by the ceiling: BYTECODE_MAX_REGISTERS
	// is 65535, so the obvious local would be a 512KB frame on a fiber stack
	// that starts at 64KB.
	uint16_t count = baseline->frameSlots;
	Value *registers = calloc(count == 0 ? 1 : count, sizeof(Value));
	ASSERT(registers != NULL);
	for (uint16_t i = 0; i < count; i++) {
		registers[i] = tagPtr(Handles.nil.raw);
	}
	for (uint16_t i = 0; i < frame->slotCount; i++) {
		const DeoptSlot *slot = &frame->slots[i];
		ASSERT(slot->bytecodeRegister < count);
		registers[slot->bytecodeRegister] = readSlot(slot, optimized, saved);
	}

	uint16_t alignment = 16 / sizeof(Value);
	uint64_t frameBytes = (uint64_t) (((count + alignment - 1) / alignment)
		* alignment * sizeof(Value));
	const void *target = baseline->entry + baseline->machineOffsetAt[frame->bci];
	Value answer = jitResumeAt(target, registers, count, frameBytes);
	free(registers);
	return answer;
}
