// Linear scan register allocation with splitting (Wimmer and Moessenboeck),
// over the LIR, with the integer and float banks allocated independently.
//
// WHY THIS ALGORITHM. The alternative worth considering is graph colouring,
// which produces better code and costs a great deal more compile time. This is
// a JIT: the compilation happens while the program is waiting. Linear scan with
// splitting and lifetime holes is what the literature settled on for exactly
// this position, and the "with splitting" half is what separates it from the
// naive version, which spills a value for its whole life because one loop had
// no register free.
//
// THE POSITION NUMBERING is the whole trick and it is worth stating plainly.
// Instructions get EVEN positions; the odd numbers between them are where splits
// and resolution moves live. A definition at instruction p starts its interval
// at p + 1, and a use at p ends at p. So a value defined and immediately killed
// occupies [p+1, p+1) and never collides with its own operands, and an interval
// split at an odd position is unambiguously between two instructions.
//
// WHAT LEAVES THIS FILE. Every operand names a physical register. Splitting
// means one virtual register may live in several places over its life, and
// rather than expose that, the rewrite at the end replaces each operand with the
// location in force at that position and inserts the moves. Nothing downstream
// has to ask.

#include "jit/RegAlloc.h"
#include "jit/Deopt.h"
#include "core/Assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POSITION_MAX INT32_MAX

typedef struct {
	LirFunction *function;
	RegAllocStats stats;
	// The intervals of one bank, being allocated. Separate passes per bank, so
	// these are reused.
	LirBank bank;
	const uint8_t *allocatable;
	uint8_t allocatableCount;
	// Physical register -> the interval currently holding it, indexed by the
	// register NUMBER rather than by a position in `allocatable`, because a
	// fixed interval names a register the allocator was never offered.
	int32_t freeUntil[64];
	int32_t nextUse[64];
	int32_t blocked[64];
	LirInterval *unhandled;   // sorted by start
	LirInterval *active;
	LirInterval *inactive;
	LirInterval *handled;
	// Every interval, in creation order, for the rewrite and the verifier.
	LirInterval **all;
	uint32_t allCount, allCapacity;
	// Virtual register -> its first interval. Split intervals chain from it.
	LirInterval **byVreg;
	// IR value id -> virtual register, handed over by the lowering so a deopt
	// state can be translated from values to locations. See buildDeoptSites.
	uint32_t *deoptVregOf;
	uint32_t deoptVregCapacity;
	// The allocation cannot be completed at this register-pool size. Not an
	// error in the input and not a bug: an instruction that needs a destination
	// and an operand in registers at adjacent positions needs TWO, and no amount
	// of splitting conjures a third. Reached only under ST_SSA_REGS, where the
	// pool is deliberately shrunk below what the LIR can be served with.
	//
	// It has to be DETECTED rather than left to happen, because the shape it
	// takes otherwise is a scan that hands an interval back to itself forever.
	_Bool infeasible;
	// One frame slot for breaking a CYCLE of resolution moves, allocated the
	// first time one is found and LIR_NO_SLOT otherwise, so a method with no
	// cycle pays nothing for the possibility.
	//
	// It never needs to appear in a frame map, and that is a property of where it
	// is used rather than luck: the store into it and the load out of it are
	// emitted as consecutive instructions at ONE insertion point, with nothing
	// between them but other resolution moves. No safepoint can fall inside that
	// window, so no collector ever sees the slot occupied.
	int32_t exchangeSlot;
} Allocator;


// ---------------------------------------------------------------------------
// Intervals
// ---------------------------------------------------------------------------

static LirInterval *newInterval(Allocator *allocator, uint32_t vreg, LirBank bank,
	SlotKind kind)
{
	LirInterval *interval = lirAlloc(allocator->function, sizeof(LirInterval));
	interval->vreg = vreg;
	interval->bank = (uint8_t) bank;
	interval->kind = (uint8_t) kind;
	interval->reg = LIR_NO_REG;
	interval->spillSlot = LIR_NO_SLOT;
	if (allocator->allCount == allocator->allCapacity) {
		allocator->allCapacity = allocator->allCapacity == 0 ? 64
			: allocator->allCapacity * 2;
		allocator->all = realloc(allocator->all,
			allocator->allCapacity * sizeof(LirInterval *));
		ASSERT(allocator->all != NULL);
	}
	allocator->all[allocator->allCount++] = interval;
	return interval;
}


static int32_t intervalStart(const LirInterval *interval)
{
	return interval->ranges != NULL ? interval->ranges->from : POSITION_MAX;
}


static int32_t intervalEnd(const LirInterval *interval)
{
	int32_t end = -1;
	for (const LirRange *range = interval->ranges; range != NULL;
			range = range->next) {
		end = range->to;
	}
	return end;
}


static _Bool intervalCovers(const LirInterval *interval, int32_t position)
{
	for (const LirRange *range = interval->ranges; range != NULL;
			range = range->next) {
		if (position >= range->from && position < range->to) {
			return 1;
		}
	}
	return 0;
}


// The first position at which two intervals are both live, or POSITION_MAX.
// Walked as two ordered range lists, which is what makes it linear.
static int32_t firstIntersection(const LirInterval *a, const LirInterval *b)
{
	const LirRange *ra = a->ranges;
	const LirRange *rb = b->ranges;
	while (ra != NULL && rb != NULL) {
		if (ra->to <= rb->from) {
			ra = ra->next;
		} else if (rb->to <= ra->from) {
			rb = rb->next;
		} else {
			return ra->from > rb->from ? ra->from : rb->from;
		}
	}
	return POSITION_MAX;
}


// Add [from, to) to an interval, MERGING with the range that starts at `to`
// when there is one.
//
// Built BACKWARDS, one block at a time from the end of the function, which is
// why this prepends and merges rather than appending: liveness flows against
// control flow, so the earliest range is the last one learned.
static void addRange(Allocator *allocator, LirInterval *interval, int32_t from,
	int32_t to)
{
	if (from >= to) {
		return;
	}
	if (interval->ranges != NULL && interval->ranges->from <= to) {
		// Overlaps or abuts the first range: widen it instead of adding one.
		if (from < interval->ranges->from) {
			interval->ranges->from = from;
		}
		if (to > interval->ranges->to) {
			interval->ranges->to = to;
		}
		return;
	}
	LirRange *range = lirAlloc(allocator->function, sizeof(LirRange));
	range->from = from;
	range->to = to;
	range->next = interval->ranges;
	interval->ranges = range;
}


static void addUse(Allocator *allocator, LirInterval *interval, int32_t position,
	_Bool needsRegister)
{
	if (interval->uses != NULL && interval->uses->position == position) {
		interval->uses->needsRegister |= needsRegister;
		return;
	}
	LirUse *use = lirAlloc(allocator->function, sizeof(LirUse));
	use->position = position;
	use->needsRegister = needsRegister;
	use->next = interval->uses;
	interval->uses = use;
}


// The first use at or after `position` that REQUIRES a register.
static int32_t nextRegisterUse(const LirInterval *interval, int32_t position)
{
	for (const LirUse *use = interval->uses; use != NULL; use = use->next) {
		if (use->position >= position && use->needsRegister) {
			return use->position;
		}
	}
	return POSITION_MAX;
}


// ---------------------------------------------------------------------------
// Liveness, and the intervals it builds
// ---------------------------------------------------------------------------
//
// Backwards over the blocks in reverse layout order, with a live set per block.
// A value live on entry to a successor is live to the END of this block, which
// is what gives an interval spanning a loop its range across the back edge
// without any special case for loops.

typedef struct {
	uint64_t *words;
	uint32_t wordCount;
} LiveSet;


static void liveSetInit(LiveSet *set, uint32_t vregCount)
{
	set->wordCount = (vregCount + 63) / 64;
	set->words = calloc(set->wordCount == 0 ? 1 : set->wordCount, sizeof(uint64_t));
	ASSERT(set->words != NULL);
}


static void liveSetAdd(LiveSet *set, uint32_t vreg)
{
	set->words[vreg / 64] |= (uint64_t) 1 << (vreg % 64);
}


static void liveSetRemove(LiveSet *set, uint32_t vreg)
{
	set->words[vreg / 64] &= ~((uint64_t) 1 << (vreg % 64));
}


static _Bool liveSetHas(const LiveSet *set, uint32_t vreg)
{
	return (set->words[vreg / 64] & ((uint64_t) 1 << (vreg % 64))) != 0;
}


static _Bool liveSetUnion(LiveSet *into, const LiveSet *from)
{
	_Bool changed = 0;
	for (uint32_t i = 0; i < into->wordCount; i++) {
		uint64_t merged = into->words[i] | from->words[i];
		changed |= merged != into->words[i];
		into->words[i] = merged;
	}
	return changed;
}


static LirInterval *intervalFor(Allocator *allocator, uint32_t vreg)
{
	if (allocator->byVreg[vreg] == NULL) {
		allocator->byVreg[vreg] = newInterval(allocator, vreg,
			(LirBank) allocator->function->vregBank[vreg],
			(SlotKind) allocator->function->vregKind[vreg]);
	}
	return allocator->byVreg[vreg];
}


// A FIXED interval: a physical register that is unavailable over a range,
// because a call clobbers it or an instruction demands it.
//
// Modelled as an interval rather than as a check at the allocation site, and
// that is what makes it cost nothing extra: the scan already refuses to give a
// register to an interval that intersects one holding it, so a call becomes
// "these registers are held by somebody" and needs no separate rule.
static LirInterval *fixedInterval(Allocator *allocator, LirInterval **fixed,
	LirBank bank, uint8_t reg)
{
	if (fixed[reg] == NULL) {
		fixed[reg] = newInterval(allocator, LIR_NO_VREG, bank, SLOT_DEAD);
		fixed[reg]->fixed = 1;
		fixed[reg]->reg = (int16_t) reg;
	}
	return fixed[reg];
}


static _Bool isCallerSaved(const Abi *abi, LirBank bank, uint8_t reg)
{
	if (bank == LIR_BANK_INT) {
		for (uint8_t i = 0; i < abi->callerSavedCount; i++) {
			if (abi->callerSaved[i] == reg) {
				return 1;
			}
		}
		return 0;
	}
	// The float file the other way round: everything not listed as callee-saved
	// is caller-saved. Under System V that list is empty and every SSE register
	// is destroyed by a call, which is the correct answer there and the one an
	// allocator that assumed otherwise would get silently wrong.
	for (uint8_t i = 0; i < abi->calleeSavedFloatCount; i++) {
		if (abi->calleeSavedFloat[i] == reg) {
			return 0;
		}
	}
	return 1;
}


// Which registers an instruction destroys beyond its own destination.
//
// Two sources, and the second is easy to forget: a CALL destroys every
// caller-saved register of both banks, and an x86 DIVISION destroys the
// remainder register. The second is a machine fact leaking into a neutral file,
// so it is expressed the neutral way -- "this op clobbers these ABI-named
// registers" -- and a target without the constraint simply lists none.
static void addClobbers(Allocator *allocator, LirInterval *fixed[][64],
	const LirInstruction *instruction)
{
	const Abi *abi = allocator->function->abi;
	if (!lirOpClobbers((LirOp) instruction->op)) {
		return;
	}
	int32_t position = instruction->position;
	// BOTH banks in one pass. Liveness does not care which register file a value
	// lives in, so the intervals are built once; only the SCAN is per bank.
	for (int bank = 0; bank < LIR_BANK_COUNT; bank++) {
		const uint8_t *set = bank == LIR_BANK_INT
			? abi->allocatableInteger : abi->allocatableFloat;
		uint8_t count = bank == LIR_BANK_INT
			? abi->allocatableIntegerCount : abi->allocatableFloatCount;
		for (uint8_t i = 0; i < count; i++) {
			if (!isCallerSaved(abi, (LirBank) bank, set[i])) {
				continue;
			}
			LirInterval *interval = fixedInterval(allocator, fixed[bank],
				(LirBank) bank, set[i]);
			// [position, position + 1): held ACROSS the instruction and free
			// again afterwards, so a value the call itself defines can use it.
			addRange(allocator, interval, position, position + 1);
		}
	}
}


static void buildIntervals(Allocator *allocator, LirInterval *fixed[][64])
{
	LirFunction *function = allocator->function;
	uint32_t blockCount = function->orderCount;
	LiveSet *liveIn = calloc(blockCount, sizeof(LiveSet));
	ASSERT(liveIn != NULL);
	for (uint32_t i = 0; i < blockCount; i++) {
		liveSetInit(&liveIn[i], function->vregCount);
	}

	// Block indices, for reaching a successor's live-in from a predecessor.
	uint32_t *indexOfBlock = calloc(function->blockCount, sizeof(uint32_t));
	ASSERT(indexOfBlock != NULL);
	for (uint32_t i = 0; i < blockCount; i++) {
		indexOfBlock[function->order[i]->id] = i;
	}

	// The live sets first, to a fixpoint. A loop needs the second pass: the
	// header's live-in is not known when the body is first walked.
	LiveSet live;
	liveSetInit(&live, function->vregCount);
	_Bool changed = 1;
	while (changed) {
		changed = 0;
		for (int32_t i = (int32_t) blockCount - 1; i >= 0; i--) {
			LirBlock *block = function->order[i];
			memset(live.words, 0, live.wordCount * sizeof(uint64_t));
			for (uint8_t s = 0; s < block->succCount; s++) {
				liveSetUnion(&live, &liveIn[indexOfBlock[block->succs[s]->id]]);
			}
			// Walk the block backwards. The list is singly linked, so gather it.
			uint32_t count = 0;
			for (LirInstruction *it = block->first; it != NULL; it = it->next) {
				count++;
			}
			LirInstruction **ordered = calloc(count == 0 ? 1 : count,
				sizeof(LirInstruction *));
			ASSERT(ordered != NULL);
			uint32_t index = 0;
			for (LirInstruction *it = block->first; it != NULL; it = it->next) {
				ordered[index++] = it;
			}
			for (int32_t k = (int32_t) count - 1; k >= 0; k--) {
				LirInstruction *instruction = ordered[k];
				if (instruction->dst != LIR_NO_VREG) {
					liveSetRemove(&live, instruction->dst);
				}
				for (uint8_t a = 0; a < instruction->argCount; a++) {
					if (instruction->args[a] != LIR_NO_VREG) {
						liveSetAdd(&live, instruction->args[a]);
					}
				}
			}
			free(ordered);
			changed |= liveSetUnion(&liveIn[i], &live);
		}
	}

	// Now the ranges, one backward pass over the blocks in layout order.
	for (int32_t i = (int32_t) blockCount - 1; i >= 0; i--) {
		LirBlock *block = function->order[i];
		memset(live.words, 0, live.wordCount * sizeof(uint64_t));
		for (uint8_t s = 0; s < block->succCount; s++) {
			liveSetUnion(&live, &liveIn[indexOfBlock[block->succs[s]->id]]);
		}
		// Everything live out of this block is live across the WHOLE of it, and
		// gets trimmed below by its definition. This is what gives a
		// loop-carried value its range across the back edge with no special case.
		for (uint32_t vreg = 0; vreg < function->vregCount; vreg++) {
			if (liveSetHas(&live, vreg)) {
				addRange(allocator, intervalFor(allocator, vreg), block->from,
					block->to);
			}
		}

		uint32_t count = 0;
		for (LirInstruction *it = block->first; it != NULL; it = it->next) {
			count++;
		}
		LirInstruction **ordered = calloc(count == 0 ? 1 : count,
			sizeof(LirInstruction *));
		ASSERT(ordered != NULL);
		uint32_t index = 0;
		for (LirInstruction *it = block->first; it != NULL; it = it->next) {
			ordered[index++] = it;
		}

		for (int32_t k = (int32_t) count - 1; k >= 0; k--) {
			LirInstruction *instruction = ordered[k];
			int32_t position = instruction->position;
			addClobbers(allocator, fixed, instruction);

			if (instruction->dst != LIR_NO_VREG) {
				LirInterval *interval = intervalFor(allocator, instruction->dst);
				// The definition ENDS whatever range reached back to here: the
				// value does not exist before its own definition.
				if (interval->ranges != NULL
						&& interval->ranges->from <= position) {
					interval->ranges->from = position + 1;
				} else {
					// Defined and never used. It still occupies its destination
					// for one position, or the register would look free to an
					// interval whose range starts here -- and that position is
					// position + 1, the same odd slot a LIVE definition starts
					// at. Starting it at `position` instead would put it where
					// the OPERANDS of this very instruction live, so the rewrite
					// would find no location at the definition and the call's
					// clobbers would look like a conflict.
					addRange(allocator, interval, position + 1, position + 2);
				}
				addUse(allocator, interval, position + 1, 1);
				liveSetRemove(&live, instruction->dst);
			}
			// A DEOPTIMIZATION STATE IS A USE of everything it names, and
			// missing that is not a missed optimization: the value's interval
			// ends at its last ORDINARY use, so at a later deopt point it lives
			// nowhere and the state cannot say where to find it. Measured over
			// the kernel before this was here: 53961 of 63651 states could not
			// be described, which is to say the normal case rather than an edge.
			//
			// needsRegister is 0, deliberately. Deoptimization reads the value
			// once, on a path that is by definition cold, so a spill slot serves
			// it perfectly; demanding a register would push real work out of the
			// register file to satisfy a path that mostly never runs.
			if (instruction->deopt != NULL) {
				DeoptState *state = instruction->deopt;
				for (uint16_t f = 0; f < state->frameCount; f++) {
					DeoptFrame *frame = &state->frames[f];
					for (uint16_t v = 0; v < frame->slotCount; v++) {
						IrValue *named = frame->slotValue[v];
						if (named == NULL
								|| named->id >= allocator->deoptVregCapacity) {
							continue;
						}
						uint32_t vreg = allocator->deoptVregOf[named->id];
						if (vreg == LIR_NO_VREG) {
							continue;
						}
						LirInterval *interval = intervalFor(allocator, vreg);
						addRange(allocator, interval, block->from, position + 1);
						addUse(allocator, interval, position, 0);
						liveSetAdd(&live, vreg);
					}
				}
			}
			for (uint8_t a = 0; a < instruction->argCount; a++) {
				uint32_t vreg = instruction->args[a];
				if (vreg == LIR_NO_VREG) {
					continue;
				}
				LirInterval *interval = intervalFor(allocator, vreg);
				// UP TO AND INCLUDING the use. Ranges are half-open, so a read
				// at `position` needs `position + 1` as the end: ending at
				// `position` leaves the operand uncovered exactly where it is
				// read, and the rewrite then finds no location for it. The
				// value defined by this same instruction starts at position + 1
				// and still does not overlap, which is the whole reason
				// definitions are odd and uses are even.
				addRange(allocator, interval, block->from, position + 1);
				addUse(allocator, interval, position, 1);
				liveSetAdd(&live, vreg);
			}
		}
		free(ordered);
	}

	for (uint32_t i = 0; i < blockCount; i++) {
		free(liveIn[i].words);
	}
	free(liveIn);
	free(live.words);
	free(indexOfBlock);
}


// ---------------------------------------------------------------------------
// The scan
// ---------------------------------------------------------------------------

static void listInsertSorted(LirInterval **list, LirInterval *interval)
{
	LirInterval **at = list;
	while (*at != NULL && intervalStart(*at) <= intervalStart(interval)) {
		at = &(*at)->next;
	}
	interval->next = *at;
	*at = interval;
}


static void listRemove(LirInterval **list, LirInterval *interval)
{
	while (*list != NULL) {
		if (*list == interval) {
			*list = interval->next;
			interval->next = NULL;
			return;
		}
		list = &(*list)->next;
	}
}


static int32_t newSpillSlot(Allocator *allocator)
{
	int32_t slot = allocator->function->frameSlots++;
	allocator->stats.spillSlots++;
	return slot;
}


// Split `interval` at `position`, answering the second half. The two halves are
// chained, so the location of a vreg at a position is found by walking.
//
// `position` must be ODD, that is between two instructions: splitting ON an
// instruction would leave it ambiguous whether the operands of that instruction
// belong to the first half or the second.
static LirInterval *splitInterval(Allocator *allocator, LirInterval *interval,
	int32_t position)
{
	ASSERT(position % 2 == 1);
	LirInterval *tail = newInterval(allocator, interval->vreg,
		(LirBank) interval->bank, (SlotKind) interval->kind);
	tail->splitParent = interval->splitParent != NULL
		? interval->splitParent : interval;
	tail->splitNext = interval->splitNext;
	interval->splitNext = tail;
	allocator->stats.splits++;

	// Ranges: everything after `position` moves across, and a range straddling
	// it is cut in two.
	LirRange **link = &interval->ranges;
	while (*link != NULL) {
		LirRange *range = *link;
		if (range->to <= position) {
			link = &range->next;
			continue;
		}
		if (range->from >= position) {
			*link = NULL;                 // this range and the rest move
			// Append in order.
			LirRange **tailLink = &tail->ranges;
			while (*tailLink != NULL) { tailLink = &(*tailLink)->next; }
			*tailLink = range;
			break;
		}
		// Straddles: cut.
		LirRange *second = lirAlloc(allocator->function, sizeof(LirRange));
		second->from = position;
		second->to = range->to;
		second->next = range->next;
		range->to = position;
		range->next = NULL;
		LirRange **tailLink = &tail->ranges;
		while (*tailLink != NULL) { tailLink = &(*tailLink)->next; }
		*tailLink = second;
		break;
	}

	// Uses after the split point move too.
	LirUse **useLink = &interval->uses;
	LirUse *moved = NULL;
	while (*useLink != NULL) {
		LirUse *use = *useLink;
		if (use->position >= position) {
			*useLink = use->next;
			use->next = moved;
			moved = use;
			continue;
		}
		useLink = &use->next;
	}
	// `moved` came out reversed; the list is kept ascending.
	while (moved != NULL) {
		LirUse *next = moved->next;
		LirUse **at = &tail->uses;
		while (*at != NULL && (*at)->position < moved->position) {
			at = &(*at)->next;
		}
		moved->next = *at;
		*at = moved;
		moved = next;
	}
	return tail;
}


// Spill an interval: it gets a frame slot instead of a register, and is split
// before its next use that REQUIRES one, so the reload happens there rather
// than the value living in memory for the rest of its life. That is the whole
// difference between this and naive linear scan.
static void spillInterval(Allocator *allocator, LirInterval *interval)
{
	LirInterval *root = interval->splitParent != NULL
		? interval->splitParent : interval;
	if (root->spillSlot == LIR_NO_SLOT) {
		root->spillSlot = newSpillSlot(allocator);
	}
	interval->spillSlot = root->spillSlot;
	interval->reg = LIR_NO_REG;
	allocator->stats.spilled++;
}


// Splits land BETWEEN instructions, so the usable stretch below `position` ends
// at the odd position under it. ONE definition of that rounding, because three
// copies of it is three chances to round the wrong way at an even boundary.
static int32_t splitPointBelow(int32_t position)
{
	if (position == POSITION_MAX) {
		return POSITION_MAX;
	}
	return position % 2 == 0 ? position - 1 : position;
}


// Cut an interval at `position` and spill everything from there on, then give
// the spilled part a register again just before its next use that needs one.
//
// THE HEAD KEEPS ITS REGISTER. That is the difference between splitting and
// simply spilling, and it is not a refinement: the head holds the DEFINITION,
// so an interval spilled whole has nowhere to write its own value. The spilled
// middle is what lives in memory, and it lives there only until the next use.
static void splitAndSpill(Allocator *allocator, LirInterval *interval,
	int32_t position)
{
	int32_t at = position % 2 == 0 ? position - 1 : position;
	LirInterval *tail = interval;
	if (at > intervalStart(interval)) {
		tail = splitInterval(allocator, interval, at);
	}
	spillInterval(allocator, tail);

	int32_t use = nextRegisterUse(tail, intervalStart(tail));
	if (use == POSITION_MAX) {
		return;   // never needed in a register again: memory is where it ends
	}
	int32_t before = use % 2 == 0 ? use - 1 : use;
	if (before > intervalStart(tail)) {
		LirInterval *reloaded = splitInterval(allocator, tail, before);
		listInsertSorted(&allocator->unhandled, reloaded);
		return;
	}
	// There is no spilled stretch at all: the first thing this part does is a
	// use that needs a register, so spilling it bought nothing. Handing it back
	// to the scan would make no progress and the scan would hand it back again,
	// forever. The pool is too small for this LIR, and saying so is the answer.
	allocator->infeasible = 1;
}


static _Bool tryAllocateFree(Allocator *allocator, LirInterval *current)
{
	for (int i = 0; i < 64; i++) {
		allocator->freeUntil[i] = -1;
	}
	for (uint8_t i = 0; i < allocator->allocatableCount; i++) {
		allocator->freeUntil[allocator->allocatable[i]] = POSITION_MAX;
	}
	for (LirInterval *it = allocator->active; it != NULL; it = it->next) {
		if (it->reg >= 0) {
			allocator->freeUntil[it->reg] = 0;
		}
	}
	for (LirInterval *it = allocator->inactive; it != NULL; it = it->next) {
		if (it->reg < 0) {
			continue;
		}
		int32_t meet = firstIntersection(it, current);
		if (meet < allocator->freeUntil[it->reg]) {
			allocator->freeUntil[it->reg] = meet;
		}
	}

	int best = -1;
	for (uint8_t i = 0; i < allocator->allocatableCount; i++) {
		uint8_t reg = allocator->allocatable[i];
		if (best < 0 || allocator->freeUntil[reg] > allocator->freeUntil[best]) {
			best = reg;
		}
	}
	if (best < 0 || allocator->freeUntil[best] <= intervalStart(current)) {
		return 0;
	}
	current->reg = (int16_t) best;
	if (intervalEnd(current) <= allocator->freeUntil[best]) {
		return 1;
	}
	// Fits only up to there. Take it, and split the rest off to be handled as
	// its own interval when the scan reaches it.
	int32_t at = allocator->freeUntil[best];
	if (at % 2 == 0) {
		at--;   // splits live between instructions
	}
	if (at <= intervalStart(current)) {
		current->reg = LIR_NO_REG;
		return 0;
	}
	LirInterval *tail = splitInterval(allocator, current, at);
	listInsertSorted(&allocator->unhandled, tail);
	return 1;
}


static void allocateBlocked(Allocator *allocator, LirInterval *current)
{
	for (int i = 0; i < 64; i++) {
		allocator->nextUse[i] = -1;
	}
	for (uint8_t i = 0; i < allocator->allocatableCount; i++) {
		allocator->nextUse[allocator->allocatable[i]] = POSITION_MAX;
	}
	int32_t position = intervalStart(current);

	// TWO different questions, and conflating them is what the residual bug
	// was.
	//
	//   nextUse[reg]  how soon the current holder needs it again. Decides WHICH
	//                 register is cheapest to take.
	//   blocked[reg]  the position past which this register cannot be held AT
	//                 ALL, whatever we are willing to spill.
	//
	// An ACTIVE ordinary interval blocks nothing: it can be split and spilled,
	// which is what the eviction below does. A FIXED one blocks from here,
	// because it models the machine itself using the register. And an INACTIVE
	// one -- fixed or not -- blocks from the point the two meet, because THIS
	// ALLOCATOR NEVER SPILLS AN INTERVAL THAT IS CURRENTLY INACTIVE. That last
	// clause is the whole of it: treating an inactive ordinary holder as merely
	// something to split against assumes it could be evicted instead, and
	// nothing here ever evicts one.
	for (int i = 0; i < 64; i++) {
		allocator->blocked[i] = POSITION_MAX;
	}
	for (LirInterval *it = allocator->active; it != NULL; it = it->next) {
		if (it->reg < 0) {
			continue;
		}
		int32_t use = it->fixed ? position : nextRegisterUse(it, position);
		if (use < allocator->nextUse[it->reg]) {
			allocator->nextUse[it->reg] = use;
		}
		if (it->fixed && position < allocator->blocked[it->reg]) {
			allocator->blocked[it->reg] = position;
		}
	}
	for (LirInterval *it = allocator->inactive; it != NULL; it = it->next) {
		if (it->reg < 0) {
			continue;
		}
		int32_t meet = firstIntersection(it, current);
		if (meet == POSITION_MAX) {
			continue;
		}
		int32_t use = it->fixed ? meet : nextRegisterUse(it, position);
		if (use < allocator->nextUse[it->reg]) {
			allocator->nextUse[it->reg] = use;
		}
		if (meet < allocator->blocked[it->reg]) {
			allocator->blocked[it->reg] = meet;
		}
	}

	// A register is only a candidate if taking it buys a stretch worth having.
	// Splits land BETWEEN instructions, so the usable stretch is up to the odd
	// position below the block; when that is not past this interval's own start
	// there is no stretch at all, and taking the register would mean holding it
	// straight through the collision. That is precisely the shape the verifier
	// reported: an inactive holder whose next range began one position after
	// the newcomer started, so the only legal split point WAS the start.
	int best = -1;
	for (uint8_t i = 0; i < allocator->allocatableCount; i++) {
		uint8_t reg = allocator->allocatable[i];
		if (splitPointBelow(allocator->blocked[reg]) <= position) {
			continue;
		}
		if (best < 0 || allocator->nextUse[reg] > allocator->nextUse[best]) {
			best = reg;
		}
	}

	int32_t ownFirstUse = nextRegisterUse(current, position);
	// A value spilled from its own START is only expressible when its first
	// register use comes LATER: the definition itself needs somewhere to be
	// written. So this branch is guarded on that, and when it does not hold the
	// eviction below runs even against a more urgent holder, because one of the
	// two has to give and only one of them can.
	if ((best < 0 || allocator->nextUse[best] <= ownFirstUse)
			&& ownFirstUse > position) {
		splitAndSpill(allocator, current, position);
		return;
	}
	if (best < 0) {
		// Every register is blocked from this very position and this value
		// needs one HERE. No split and no spill expresses that, so say so
		// rather than emit an allocation that overlaps.
		allocator->infeasible = 1;
		return;
	}

	// Evict the holder of `best`, which is now this interval's.
	current->reg = (int16_t) best;
	LirInterval *next = NULL;
	for (LirInterval *it = allocator->active; it != NULL; it = next) {
		next = it->next;
		if (it->reg != best || it->fixed) {
			continue;
		}
		// SPLIT HERE AND SPILL THE TAIL, not spill the whole interval. The head
		// keeps the register up to this point, and it has to: the head contains
		// the DEFINITION, which needs somewhere to be written. Spilling the
		// whole interval leaves its definition with no register at all, which
		// the verifier catches -- measured, at a one-register pool, on a method
		// as small as a single send.
		listRemove(&allocator->active, it);
		splitAndSpill(allocator, it, position);
		listInsertSorted(&allocator->handled, it);
	}
	// And take it only up to the point it is blocked. The candidate filter above
	// guarantees this split point is past the start, so it always makes progress.
	if (allocator->blocked[best] < intervalEnd(current)) {
		int32_t at = splitPointBelow(allocator->blocked[best]);
		ASSERT(at > position);
		LirInterval *tail = splitInterval(allocator, current, at);
		listInsertSorted(&allocator->unhandled, tail);
	}
}


// ST_SSA_REGS=n: pretend each bank has only the first n allocatable registers.
//
// A TOOL AND NOT A SETTING. The spill and split paths are the ones a small
// method never reaches, so on a twelve-register file they are the code least
// likely to have been executed and most likely to be wrong. Shrinking the pool
// makes an ordinary method spill, and running the whole suite at n = 1 makes
// every method spill. The previous VM had exactly this knob and it is what found
// its spill bug; the difference here is that the smallest legal value is 1 and
// means one register, where the old one read 0 as "no limit".
static uint8_t registerPoolLimit(void)
{
	static int limit = -1;
	if (limit < 0) {
		const char *text = getenv("ST_SSA_REGS");
		limit = text != NULL ? atoi(text) : 0;
		if (limit < 0) {
			limit = 0;
		}
	}
	return (uint8_t) limit;
}


// Seed the scan with the FIXED intervals of this bank.
//
// They go straight into `inactive` rather than into `unhandled`, and that is not
// an optimization: tryAllocateFree consults only active and inactive, so a fixed
// interval still sitting in `unhandled` is invisible. A value whose life starts
// before the first call would then be handed a caller-saved register and keep it
// across the call, which is a value destroyed by a callee that was entitled to
// destroy it. Measured, on a method with three sends: the receiver took a
// caller-saved register and the verifier caught it.
static void seedFixed(Allocator *allocator, LirInterval *fixed[][64], LirBank bank)
{
	for (int reg = 0; reg < 64; reg++) {
		LirInterval *interval = fixed[bank][reg];
		if (interval != NULL && interval->ranges != NULL) {
			listInsertSorted(&allocator->inactive, interval);
		}
	}
}


static void scanBank(Allocator *allocator, LirBank bank, LirInterval *fixed[][64])
{
	const Abi *abi = allocator->function->abi;
	allocator->bank = bank;
	allocator->allocatable = bank == LIR_BANK_INT ? abi->allocatableInteger
		: abi->allocatableFloat;
	allocator->allocatableCount = bank == LIR_BANK_INT
		? abi->allocatableIntegerCount : abi->allocatableFloatCount;
	uint8_t limit = registerPoolLimit();
	if (limit > 0 && limit < allocator->allocatableCount) {
		allocator->allocatableCount = limit;
	}
	// A bank with no allocatable registers and values that need them cannot be
	// served, and saying so beats producing code that uses register 0 of a file
	// the ABI never described.
	ASSERT(allocator->allocatableCount > 0 || allocator->unhandled == NULL);

	allocator->active = NULL;
	allocator->inactive = NULL;
	allocator->handled = NULL;
	seedFixed(allocator, fixed, bank);

	while (allocator->unhandled != NULL && !allocator->infeasible) {
		LirInterval *current = allocator->unhandled;
		allocator->unhandled = current->next;
		current->next = NULL;
		int32_t position = intervalStart(current);

		LirInterval *next = NULL;
		for (LirInterval *it = allocator->active; it != NULL; it = next) {
			next = it->next;
			if (intervalEnd(it) <= position) {
				listRemove(&allocator->active, it);
				listInsertSorted(&allocator->handled, it);
			} else if (!intervalCovers(it, position)) {
				listRemove(&allocator->active, it);
				listInsertSorted(&allocator->inactive, it);
			}
		}
		for (LirInterval *it = allocator->inactive; it != NULL; it = next) {
			next = it->next;
			if (intervalEnd(it) <= position) {
				listRemove(&allocator->inactive, it);
				listInsertSorted(&allocator->handled, it);
			} else if (intervalCovers(it, position)) {
				listRemove(&allocator->inactive, it);
				listInsertSorted(&allocator->active, it);
			}
		}

		if (!tryAllocateFree(allocator, current)) {
			allocateBlocked(allocator, current);
		}
		if (current->reg >= 0) {
			listInsertSorted(&allocator->active, current);
			if (!isCallerSaved(abi, bank, (uint8_t) current->reg)) {
				allocator->stats.calleeSavedUsed[bank] |=
					(uint32_t) 1 << current->reg;
			}
		} else {
			listInsertSorted(&allocator->handled, current);
		}
	}
}


// ---------------------------------------------------------------------------
// Resolution and the rewrite
// ---------------------------------------------------------------------------

// Which split of `vreg` is in force at `position`.
//
// The chain is short by construction -- an interval is split only when a
// register runs out -- so a walk beats an index that would have to be rebuilt
// after every split.
static LirInterval *locationAt(Allocator *allocator, uint32_t vreg,
	int32_t position)
{
	for (LirInterval *it = allocator->byVreg[vreg]; it != NULL;
			it = it->splitNext) {
		if (intervalCovers(it, position)) {
			return it;
		}
	}
	return NULL;
}


// ---------------------------------------------------------------------------
// THE RESOLUTION MOVES ARE A PARALLEL COPY, NOT A SEQUENCE
// ---------------------------------------------------------------------------
//
// Several values can change location at ONE point, and every one of those moves
// is supposed to happen simultaneously. Emitted one at a time in whatever order
// the loop that found them ran, they do not: a move that writes a register
// destroys what a later move was going to read out of it.
//
// Measured, and it took the tier-1 oracle to see it, twice. A loop's counter was
// stored to its spill slot AFTER another value had been loaded into the register
// it lived in, so the slot received the wrong value; and one iteration earlier a
// reload had overwritten a register a pending spill still had to read. Both are
// silent: the allocation is internally consistent, the intervals do not overlap,
// and the verifier is right to pass. Only the ORDER is wrong, and the answer is
// simply different.
//
// So the moves at a point are COLLECTED and then emitted in an order that cannot
// clobber:
//
//   1. every SPILL first        -- they read registers and write memory only
//   2. then the register-to-register moves, topologically ordered
//   3. then every RELOAD        -- they write registers and read memory only
//
// A true CYCLE among the register-to-register moves (r1 to r2 while r2 goes to
// r1) needs a scratch register this allocator has already handed out, so it is a
// NAMED REFUSAL rather than a guess. Tier 1's code stands, which is always
// correct; emitting the moves in some order and hoping is what produced the two
// bugs above.
typedef struct {
	LirBlock *block;
	LirInstruction *before;
	const LirInterval *from;
	const LirInterval *to;
} PendingMove;

typedef struct {
	PendingMove *moves;
	uint32_t count, capacity;
} MoveList;


static void pendMove(MoveList *list, LirBlock *block, LirInstruction *before,
	const LirInterval *from, const LirInterval *to)
{
	if (from->reg == to->reg && from->spillSlot == to->spillSlot) {
		return;
	}
	if (list->count == list->capacity) {
		list->capacity = list->capacity == 0 ? 16 : list->capacity * 2;
		list->moves = realloc(list->moves, list->capacity * sizeof(PendingMove));
		ASSERT(list->moves != NULL);
	}
	PendingMove *pending = &list->moves[list->count++];
	pending->block = block;
	pending->before = before;
	pending->from = from;
	pending->to = to;
}


// A move between two LOCATIONS of the same virtual register: register to
// register, register to slot, or slot to register.
//
// Slot to slot cannot happen and is asserted rather than handled: a split only
// ever moves a value between a register and its ONE spill slot, so the two ends
// are never both memory. Handling it would need a scratch register the
// allocator has already given away.
static void insertLocationMove(Allocator *allocator, LirBlock *block,
	LirInstruction *before, const LirInterval *from, const LirInterval *to)
{
	if (from->reg == to->reg && from->spillSlot == to->spillSlot) {
		return;
	}
	allocator->stats.resolutionMoves++;
	LirOp op;
	if (from->reg >= 0 && to->reg >= 0) {
		op = LIR_MOVE;
	} else if (from->reg >= 0) {
		op = LIR_STORE_SLOT;
	} else {
		ASSERT(to->reg >= 0);
		op = LIR_LOAD_SLOT;
	}
	LirInstruction *move = lirInsertBefore(allocator->function, block, before, op);
	// The move is a fully resolved instruction: it names locations, not virtual
	// registers, so the rewrite below leaves it alone.
	move->position = before != NULL ? before->position : block->to;
	switch (op) {
	case LIR_MOVE:
		move->dst = to->vreg;
		move->args[0] = from->vreg;
		move->argCount = 1;
		move->dstReg = to->reg;
		move->argReg[0] = from->reg;
		break;
	case LIR_STORE_SLOT:
		move->args[0] = from->vreg;
		move->argCount = 1;
		move->argReg[0] = from->reg;
		move->imm = to->spillSlot;
		break;
	default:
		move->dst = to->vreg;
		move->dstReg = to->reg;
		move->imm = from->spillSlot;
		break;
	}
}


// One raw spill or reload, with the slot given rather than taken from an
// interval. What breaking a cycle needs: the value goes to a slot that is not
// its own.
static void emitSlotMove(Allocator *allocator, LirBlock *block,
	LirInstruction *before, LirOp op, uint32_t vreg, int16_t reg, int32_t slot)
{
	LirInstruction *move = lirInsertBefore(allocator->function, block, before, op);
	move->position = before != NULL ? before->position : block->to;
	move->imm = slot;
	if (op == LIR_STORE_SLOT) {
		move->args[0] = vreg;
		move->argCount = 1;
		move->argReg[0] = reg;
	} else {
		move->dst = vreg;
		move->dstReg = reg;
	}
	allocator->stats.resolutionMoves++;
}


// Emit every collected move, grouped by insertion point and ordered inside a
// group.
static void emitPendingMoves(Allocator *allocator, MoveList *list)
{
	_Bool *done = calloc(list->count == 0 ? 1 : list->count, sizeof(_Bool));
	ASSERT(done != NULL);
	for (uint32_t i = 0; i < list->count; i++) {
		if (done[i]) {
			continue;
		}
		// The group is every move at this same insertion point. Found by scanning
		// rather than by sorting: the list is short and a comparison over pointers
		// would not be a meaningful order anyway.
		LirInstruction *before = list->moves[i].before;
		LirBlock *block = list->moves[i].block;
		// The one member of a cycle that was broken through the frame, whose
		// reload has to come after every other move in the group.
		uint32_t broken = list->count;

		// 1. THE SPILLS, which read registers and write memory only, so no move
		// after them can destroy what they were going to read.
		for (uint32_t k = i; k < list->count; k++) {
			PendingMove *m = &list->moves[k];
			if (done[k] || m->before != before || m->block != block) { continue; }
			if (m->from->reg >= 0 && m->to->reg < 0) {
				insertLocationMove(allocator, block, before, m->from, m->to);
				done[k] = 1;
			}
		}

		// 2. THE REGISTER-TO-REGISTER MOVES, one at a time, and only ever one
		// whose destination register nothing still pending has to read.
		for (;;) {
			_Bool progress = 0;
			uint32_t stuck = list->count;
			for (uint32_t k = i; k < list->count; k++) {
				PendingMove *m = &list->moves[k];
				if (done[k] || m->before != before || m->block != block) { continue; }
				if (m->from->reg < 0 || m->to->reg < 0) { continue; }
				_Bool blocked = 0;
				for (uint32_t j = i; j < list->count && !blocked; j++) {
					PendingMove *other = &list->moves[j];
					if (j == k || done[j] || other->before != before
							|| other->block != block || other->from->reg < 0) {
						continue;
					}
					blocked = other->from->reg == m->to->reg
						&& other->from->bank == m->to->bank;
				}
				if (blocked) {
					stuck = k;
					continue;
				}
				insertLocationMove(allocator, block, before, m->from, m->to);
				done[k] = 1;
				progress = 1;
			}
			if (progress) {
				continue;
			}
			if (stuck == list->count) {
				break;
			}
			// A CYCLE (r1 to r2 while r2 goes to r1), and it is broken THROUGH THE
			// FRAME rather than through a scratch register, because the allocator
			// has already handed every register out. One member's source is stored
			// to a slot of its own; the register it held is then free, so the rest
			// of the cycle proceeds; and that member finishes as a RELOAD out of
			// the slot, emitted after everything else in the group.
			//
			// Measured over 17977 real methods: 7 contain one. Refusing instead
			// would cost seven compilations to avoid two instructions.
			ASSERT(broken == list->count);
			if (allocator->exchangeSlot == LIR_NO_SLOT) {
				allocator->exchangeSlot = allocator->function->frameSlots++;
			}
			PendingMove *m = &list->moves[stuck];
			emitSlotMove(allocator, block, before, LIR_STORE_SLOT, m->from->vreg,
				m->from->reg, allocator->exchangeSlot);
			done[stuck] = 1;
			broken = stuck;
		}

		// 3. THE RELOADS, which write registers and read memory only, so nothing
		// they overwrite was still needed as a source.
		for (uint32_t k = i; k < list->count; k++) {
			PendingMove *m = &list->moves[k];
			if (done[k] || m->before != before || m->block != block) { continue; }
			insertLocationMove(allocator, block, before, m->from, m->to);
			done[k] = 1;
		}
		if (broken != list->count) {
			PendingMove *m = &list->moves[broken];
			emitSlotMove(allocator, block, before, LIR_LOAD_SLOT, m->to->vreg,
				m->to->reg, allocator->exchangeSlot);
		}
	}
	free(done);
}


// The instruction at a given even position, so a split point can be turned into
// a place in the instruction stream.
typedef struct {
	LirInstruction **byPosition;
	LirBlock **blockByPosition;
	int32_t count;
} PositionIndex;


static void buildPositionIndex(LirFunction *function, PositionIndex *index)
{
	int32_t highest = 0;
	for (LirBlock *block = function->blocks; block != NULL; block = block->next) {
		if (block->to > highest) {
			highest = block->to;
		}
	}
	index->count = highest / 2 + 1;
	index->byPosition = calloc(index->count, sizeof(LirInstruction *));
	index->blockByPosition = calloc(index->count, sizeof(LirBlock *));
	ASSERT(index->byPosition != NULL && index->blockByPosition != NULL);
	for (LirBlock *block = function->blocks; block != NULL; block = block->next) {
		for (LirInstruction *it = block->first; it != NULL; it = it->next) {
			index->byPosition[it->position / 2] = it;
			index->blockByPosition[it->position / 2] = block;
		}
	}
}


// Where a value's location CHANGES inside a block, put the move there.
static void resolveSplits(Allocator *allocator, PositionIndex *index,
	MoveList *pending)
{
	for (uint32_t vreg = 0; vreg < allocator->function->vregCount; vreg++) {
		for (LirInterval *it = allocator->byVreg[vreg]; it != NULL;
				it = it->splitNext) {
			LirInterval *next = it->splitNext;
			if (next == NULL) {
				continue;
			}
			int32_t at = intervalStart(next);
			if (at == POSITION_MAX || intervalEnd(it) != at) {
				// A HOLE rather than a split point: the value is dead in
				// between, so nothing has to be carried across and the second
				// half is filled by its own definition or by the block-boundary
				// resolution below.
				continue;
			}
			int32_t slot = (at + 1) / 2;
			if (slot >= index->count || index->byPosition[slot] == NULL) {
				continue;
			}
			LirBlock *block = index->blockByPosition[slot];
			LirInstruction *before = index->byPosition[slot];
			// A SPLIT AT A BLOCK'S FIRST INSTRUCTION IS A BLOCK-BOUNDARY
			// TRANSITION, and ONE move at the top of the block is the wrong answer
			// for it. The block may be reached from several predecessors that do
			// not agree about where the value is, and a move placed inside it runs
			// on every one of them: at a LOOP HEADER that means the entry edge's
			// spill executes again on every iteration, reading a register that by
			// then belongs to something else and writing it over the value in the
			// spill slot.
			//
			// Measured, and only an ORACLE found it: a float accumulator loop
			// answered 32.0 where tier 1 answered 3.0, because the loop's step was
			// overwritten by the accumulator on the second iteration. No assertion
			// fired and the allocation verifier was right to pass -- the intervals
			// were consistent, the PLACEMENT was not. It is not float-specific and
			// it is not new; it needed a loop whose header spills a loop-carried
			// value, which nothing had built before.
			//
			// So it goes to resolveEdges below, which asks the question per edge.
			if (before == block->first && block->predCount > 0) {
				continue;
			}
			pendMove(pending, block, before, it, next);
		}
	}
}


// And where it differs ACROSS AN EDGE, put the move ON THE EDGE -- which is
// either the end of the predecessor or the head of the successor, and choosing
// wrong is a wrong answer twice over.
//
// The predecessor's end serves only when it has ONE successor. With several, a
// move placed there runs on every one of them, and it runs BEFORE the terminator
// -- so it can also overwrite the register the conditional branch is about to
// read. Measured: a loop's exit edge needed its answer in a callee-saved
// register, the reload was placed before the branch, and the branch then tested
// the reloaded value instead of its condition. The loop ran exactly one
// iteration and answered 2.0 where tier 1 answered 3.0.
//
// With several successors the head of the SUCCESSOR is the edge, and it is safe
// for a reason the lowering already established: critical edges were split, so a
// successor reached from a multi-successor block has exactly one predecessor.
// That is asserted rather than assumed, because a lowering that stopped
// splitting them would make this silently wrong again.
static void resolveEdges(Allocator *allocator, MoveList *pending)
{
	LirFunction *function = allocator->function;
	for (LirBlock *block = function->blocks; block != NULL; block = block->next) {
		// THE PREDECESSOR'S LAST INSTRUCTION, not block->to - 1. The latter is the
		// ODD position between this block's last instruction and the successor's
		// first, which is exactly where a split at the boundary starts -- so
		// reading the location there answers where the value will be AFTER the
		// edge, and the move that has to happen ON the edge looks unnecessary.
		// Asking at the terminator's own position keeps the question inside this
		// block, which is where the move is going to be placed.
		int32_t exit = block->last != NULL ? block->last->position : block->to - 1;
		for (uint8_t s = 0; s < block->succCount; s++) {
			LirBlock *successor = block->succs[s];
			LirBlock *target = block;
			LirInstruction *before = block->last;
			if (block->succCount > 1) {
				ASSERT(successor->predCount <= 1);
				target = successor;
				before = successor->first;
			}
			for (uint32_t vreg = 0; vreg < function->vregCount; vreg++) {
				LirInterval *atEnd = locationAt(allocator, vreg, exit);
				LirInterval *atStart = locationAt(allocator, vreg,
					successor->from);
				if (atEnd == NULL || atStart == NULL || atEnd == atStart) {
					continue;
				}
				pendMove(pending, target, before, atEnd, atStart);
			}
		}
	}
}


// Replace every operand with the physical register in force there.
//
// A use with no register is an ALLOCATION FAILURE and not something to paper
// over: the scan splits before every use that needs one, so reaching here means
// the invariant broke, and inventing a scratch register would turn a broken
// allocation into a wrong answer somewhere else.
static _Bool rewriteOperands(Allocator *allocator)
{
	for (LirBlock *block = allocator->function->blocks; block != NULL;
			block = block->next) {
		for (LirInstruction *it = block->first; it != NULL; it = it->next) {
			if (it->dst != LIR_NO_VREG && it->dstReg == LIR_NO_REG) {
				LirInterval *interval = locationAt(allocator, it->dst,
					it->position + 1);
				if (interval == NULL || interval->reg < 0) {
					fprintf(stderr, "regalloc: v%u has no register at its "
						"definition (position %d)\n", it->dst, it->position);
					return 0;
				}
				it->dstReg = interval->reg;
			}
			for (uint8_t a = 0; a < it->argCount; a++) {
				if (it->args[a] == LIR_NO_VREG || it->argReg[a] != LIR_NO_REG) {
					continue;
				}
				LirInterval *interval = locationAt(allocator, it->args[a],
					it->position);
				if (interval == NULL || interval->reg < 0) {
					fprintf(stderr, "regalloc: v%u has no register at its use "
						"(position %d)\n", it->args[a], it->position);
					return 0;
				}
				it->argReg[a] = interval->reg;
			}
		}
	}
	return 1;
}


// ---------------------------------------------------------------------------
// The deoptimization sites
// ---------------------------------------------------------------------------
//
// Translating the optimizer's state into the runtime's, which is the ONE moment
// it can be done: before allocation there is no answer to "where does this
// value live", and after the LIR is destroyed there is nothing left to ask.
//
// The awkward case is the one that matters, and it is a value the optimizer
// DELETED. A constant folded away has no register and no slot, and a state that
// merely dropped it would rebuild a tier-1 frame with a hole where a live
// variable belongs -- a wrong answer on exactly the path nobody exercises,
// which is the whole reason Deopt.h says a state naming a deleted value can only
// be caught at compile time. It is carried as DEOPT_CONSTANT instead.
static _Bool describeValue(Allocator *allocator, IrValue *value, int32_t position,
	DeoptSlot *slot)
{
	LirFunction *function = allocator->function;
	// A materialization recipe is not a value at all; it is instructions for
	// rebuilding an object escape analysis erased. Nothing erases anything yet
	// (jit/Ir.h says so at IR_NEWCELL), so reaching one here means that changed
	// without this following, and guessing would rebuild the wrong object.
	if ((value->flags & IR_FLAG_MATERIALIZE) != 0) {
		return 0;
	}
	if (value->op == IR_CONST) {
		slot->where = DEOPT_CONSTANT;
		slot->kind = SLOT_POINTER;
		slot->constant = value->konst;
		return 1;
	}
	// The value has to have reached the LIR at all. One the lowering never
	// gave a virtual register to cannot be described, and saying so beats
	// describing it wrongly.
	if (value->id >= allocator->deoptVregCapacity) {
		return 0;
	}
	uint32_t vreg = allocator->deoptVregOf[value->id];
	if (vreg == LIR_NO_VREG) {
		return 0;
	}
	LirInterval *interval = locationAt(allocator, vreg, position);
	if (interval == NULL) {
		return 0;
	}
	slot->kind = function->vregKind[vreg];
	slot->bank = function->vregBank[vreg];
	if (interval->reg >= 0) {
		slot->where = DEOPT_IN_REGISTER;
		slot->location = interval->reg;
	} else if (interval->spillSlot != LIR_NO_SLOT) {
		slot->where = DEOPT_IN_SLOT;
		slot->location = interval->spillSlot;
	} else {
		return 0;
	}
	return 1;
}


static void buildDeoptSites(Allocator *allocator)
{
	LirFunction *function = allocator->function;
	for (LirBlock *block = function->blocks; block != NULL; block = block->next) {
		for (LirInstruction *it = block->first; it != NULL; it = it->next) {
			if (it->deopt == NULL) {
				continue;
			}
			DeoptState *state = it->deopt;
			DeoptSite *site = calloc(1, sizeof(DeoptSite));
			ASSERT(site != NULL);
			site->frameCount = state->frameCount;
			site->frames = calloc(state->frameCount, sizeof(DeoptRuntimeFrame));
			ASSERT(site->frames != NULL);
			_Bool complete = 1;
			for (uint16_t f = 0; f < state->frameCount; f++) {
				DeoptFrame *from = &state->frames[f];
				DeoptRuntimeFrame *into = &site->frames[f];
				into->unit = from->unit;
				into->baseline = function->tier1;
				into->saveBase = function->deoptSaveBase;
				into->bci = from->bci;
				into->destRegister = from->destRegister;
				into->innermost = from->innermost;
				into->slotCount = from->slotCount;
				into->slots = calloc(from->slotCount == 0 ? 1 : from->slotCount,
					sizeof(DeoptSlot));
				ASSERT(into->slots != NULL);
				for (uint16_t v = 0; v < from->slotCount; v++) {
					DeoptSlot *slot = &into->slots[v];
					slot->bytecodeRegister = from->slotRegister[v];
					slot->location = -1;
					if (!describeValue(allocator, from->slotValue[v],
							it->position, slot)) {
						complete = 0;
					}
				}
			}
			// A site that cannot describe every live register is NOT recorded.
			// Half a state is worse than none: none refuses the compilation,
			// half rebuilds a frame with a hole in it.
			if (complete) {
				it->deoptSite = site;
				if (function->deoptSiteCount == function->deoptSiteCapacity) {
					function->deoptSiteCapacity =
						function->deoptSiteCapacity == 0 ? 16
						: function->deoptSiteCapacity * 2;
					function->deoptSites = realloc(function->deoptSites,
						function->deoptSiteCapacity * sizeof(DeoptSite *));
					ASSERT(function->deoptSites != NULL);
				}
				function->deoptSites[function->deoptSiteCount++] = site;
				allocator->stats.deoptSites++;
			} else {
				lirFreeDeoptSite(site);
				allocator->stats.deoptIncomplete++;
			}
		}
	}
}


// ---------------------------------------------------------------------------
// The frame maps
// ---------------------------------------------------------------------------

// One map per instruction execution can leave from, built out of the live
// intervals at that position.
//
// THIS IS THE PAYOFF of ADR 0007 and it is worth naming: the description comes
// from the SAME liveness the allocator already needed, so it is exact. The old
// VM's back-edge poll marked every spilled temporary as a root because its
// liveness was a linear scan blind to control flow; there is nothing here for
// such an over-approximation to come from, and if one ever appears the decision
// was named and not implemented.
//
// TWO SOURCES, and missing either is silent. The SPILL SLOTS of live intervals,
// which is the obvious half. And the OUTGOING ARGUMENT BLOCK of the call
// itself, which is the half that looks like bookkeeping and is not: between the
// caller writing those slots and the callee reading them the runtime looks the
// method up, and a lookup that compiles allocates.
static void buildFrameMaps(Allocator *allocator)
{
	LirFunction *function = allocator->function;
	for (LirBlock *block = function->blocks; block != NULL; block = block->next) {
		for (LirInstruction *it = block->first; it != NULL; it = it->next) {
			if (!lirOpCanLeave((LirOp) it->op)) {
				continue;
			}
			uint16_t slotCount = function->frameSlots;
			size_t bytes = sizeof(FrameMap) + frameMapByteCount(slotCount);
			FrameMap *map = lirAlloc(function, bytes);
			map->slotCount = slotCount;
			map->byteCount = (uint16_t) frameMapByteCount(slotCount);

			// The receiver and the incoming arguments, which the prologue wrote
			// and which stay tagged for the whole method: slot 0 is what every
			// runtime helper reaches the closure through.
			for (uint16_t slot = 0; slot < function->parameterSlots; slot++) {
				frameMapSetKind(map, slot, SLOT_POINTER);
			}
			for (uint32_t vreg = 0; vreg < function->vregCount; vreg++) {
				LirInterval *interval = locationAt(allocator, vreg, it->position);
				if (interval == NULL || interval->spillSlot == LIR_NO_SLOT
						|| interval->reg >= 0) {
					continue;
				}
				frameMapSetKind(map, (uint16_t) interval->spillSlot,
					(SlotKind) interval->kind);
			}
			for (uint16_t i = 0; i < it->outgoingCount; i++) {
				frameMapSetKind(map, (uint16_t) (it->outgoingBase + i),
					(SlotKind) it->outgoingKind);
			}
			it->frameMap = map;
			allocator->stats.frameMaps++;
		}
	}
}


// ---------------------------------------------------------------------------

RegAllocStats lirAllocateRegisters(LirFunction *function)
{
	Allocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	// NOT the zero memset gave it: slot 0 is the receiver, and a cycle break that
	// wrote there would overwrite `self` -- which every runtime helper reaches
	// through frame[-1] (jit/Lir.h).
	allocator.exchangeSlot = LIR_NO_SLOT;
	allocator.function = function;
	allocator.byVreg = calloc(function->vregCount == 0 ? 1 : function->vregCount,
		sizeof(LirInterval *));
	ASSERT(allocator.byVreg != NULL);
	allocator.deoptVregOf = function->deoptVregOf;
	allocator.deoptVregCapacity = function->deoptVregCapacity;

	LirInterval *fixed[LIR_BANK_COUNT][64];
	memset(fixed, 0, sizeof(fixed));

	// ONCE, over both banks. Liveness does not care which register file a value
	// lives in, and running this per bank would add every vreg's ranges twice
	// and lean on addRange's merging to make that harmless -- true today, and
	// exactly the kind of thing that stops being true quietly.
	buildIntervals(&allocator, fixed);

	allocator.stats.intervals = allocator.allCount;

	for (int bank = 0; bank < LIR_BANK_COUNT; bank++) {
		allocator.unhandled = NULL;
		for (uint32_t i = 0; i < allocator.allCount; i++) {
			LirInterval *interval = allocator.all[i];
			if (interval->bank != bank || interval->ranges == NULL
					|| interval->fixed) {
				continue;   // fixed intervals are seeded into `inactive` instead
			}
			if (interval->splitParent != NULL) {
				continue;   // produced by a split, already placed
			}
			listInsertSorted(&allocator.unhandled, interval);
		}
		scanBank(&allocator, (LirBank) bank, fixed);
	}

	// An infeasible allocation STOPS HERE. Resolution assumes every interval
	// ended up somewhere, so running it over a half-finished allocation asserts
	// inside insertLocationMove -- a real failure reported as a crash in a
	// helper three steps away from the cause. The caller reads `failed` and
	// falls back to tier 1, which is always available.
	if (!allocator.infeasible) {
		PositionIndex index;
		buildPositionIndex(function, &index);
		// COLLECTED FIRST, EMITTED AFTER. The moves at one point are a parallel
		// copy and the order they are emitted in decides whether they compute it;
		// see the header above PendingMove for the two wrong answers that came
		// from emitting them as they were found.
		MoveList pending;
		memset(&pending, 0, sizeof(pending));
		resolveSplits(&allocator, &index, &pending);
		resolveEdges(&allocator, &pending);
		emitPendingMoves(&allocator, &pending);
		free(pending.moves);
		free(index.byPosition);
		free(index.blockByPosition);
		allocator.stats.failed = !rewriteOperands(&allocator);
		buildFrameMaps(&allocator);
		buildDeoptSites(&allocator);
	} else {
		fprintf(stderr, "regalloc: %u allocatable registers is not enough for "
			"this method\n", registerPoolLimit());
		allocator.stats.failed = 1;
	}

	function->intervals = NULL;
	for (uint32_t i = 0; i < allocator.allCount; i++) {
		allocator.all[i]->next = function->intervals;
		function->intervals = allocator.all[i];
	}

	function->calleeSavedUsed[LIR_BANK_INT] =
		allocator.stats.calleeSavedUsed[LIR_BANK_INT];
	function->calleeSavedUsed[LIR_BANK_FLOAT] =
		allocator.stats.calleeSavedUsed[LIR_BANK_FLOAT];

	RegAllocStats stats = allocator.stats;
	free(allocator.all);
	free(allocator.byVreg);
	return stats;
}


// ---------------------------------------------------------------------------
// The verifier
// ---------------------------------------------------------------------------
//
// A VERIFIER AND NOT A TEST. The failure mode of a register allocator is two
// live values sharing one register, and what that produces is a wrong answer in
// some unrelated method, arbitrarily far from the compilation that caused it.
// No end-to-end test finds that reliably. Checking the OUTPUT against its own
// invariants finds it at the compilation that caused it, on every method the
// system ever compiles, which is the same argument this repository already
// makes for asserting stackmap invariants in the codegen rather than testing
// for them.

// An interval's shape, for the message below. A conflict is only diagnosable
// with the RANGES in hand: "two values in one register" says nothing about
// which of the several ways that can happen actually did, and the range lists
// say it immediately.
static void printInterval(const LirInterval *interval)
{
	fprintf(stderr, "    v%u reg=%d slot=%d%s ranges:", interval->vreg,
		interval->reg, interval->spillSlot, interval->fixed ? " FIXED" : "");
	for (const LirRange *range = interval->ranges; range != NULL;
			range = range->next) {
		fprintf(stderr, " [%d,%d)", range->from, range->to);
	}
	fprintf(stderr, " uses:");
	for (const LirUse *use = interval->uses; use != NULL; use = use->next) {
		fprintf(stderr, " %d%s", use->position, use->needsRegister ? "r" : "");
	}
	fprintf(stderr, "%s\n", interval->splitParent != NULL ? " (a split)" : "");
}


static _Bool verifyNoSharedRegister(const LirFunction *function)
{
	for (const LirInterval *a = function->intervals; a != NULL; a = a->next) {
		if (a->reg < 0 || a->ranges == NULL) {
			continue;
		}
		for (const LirInterval *b = a->next; b != NULL; b = b->next) {
			if (b->reg != a->reg || b->bank != a->bank || b->ranges == NULL) {
				continue;
			}
			// A split of the SAME virtual register is not a conflict with
			// itself: the two halves are disjoint by construction, and this
			// catches the case where they are not.
			int32_t meet = firstIntersection(a, b);
			if (meet != POSITION_MAX) {
				fprintf(stderr, "regalloc: v%u and v%u both hold %s register %d "
					"at position %d\n", a->vreg, b->vreg,
					a->bank == LIR_BANK_FLOAT ? "float" : "integer", a->reg,
					meet);
				printInterval(a);
				printInterval(b);
				return 0;
			}
		}
	}
	return 1;
}


static _Bool verifyOperandsResolved(const LirFunction *function)
{
	for (const LirBlock *block = function->blocks; block != NULL;
			block = block->next) {
		for (const LirInstruction *it = block->first; it != NULL; it = it->next) {
			if (it->dst != LIR_NO_VREG && it->dstReg == LIR_NO_REG) {
				fprintf(stderr, "regalloc: %s at %d defines v%u with no "
					"register\n", lirOpName((LirOp) it->op), it->position,
					it->dst);
				return 0;
			}
			for (uint8_t a = 0; a < it->argCount; a++) {
				if (it->args[a] != LIR_NO_VREG && it->argReg[a] == LIR_NO_REG) {
					fprintf(stderr, "regalloc: %s at %d reads v%u with no "
						"register\n", lirOpName((LirOp) it->op), it->position,
						it->args[a]);
					return 0;
				}
			}
		}
	}
	return 1;
}


// A frame map may only describe slots that exist, and a slot it calls a POINTER
// must not be one the allocator gave to a raw value.
//
// The direction that matters is the one that is silent: a raw double described
// as a pointer is a bit pattern the collector will follow, and a double whose
// low bits happen to be 01 passes every plausibility test there is. That is
// requirement R1 of ADR 0003, and this is where it is checked rather than
// hoped for.
static _Bool verifyFrameMaps(const LirFunction *function)
{
	for (const LirBlock *block = function->blocks; block != NULL;
			block = block->next) {
		for (const LirInstruction *it = block->first; it != NULL; it = it->next) {
			if (it->frameMap == NULL) {
				if (lirOpCanLeave((LirOp) it->op)) {
					fprintf(stderr, "regalloc: %s at %d can leave and has no "
						"frame map\n", lirOpName((LirOp) it->op), it->position);
					return 0;
				}
				continue;
			}
			if (it->frameMap->slotCount > function->frameSlots) {
				fprintf(stderr, "regalloc: frame map at %d describes %u slots "
					"of a %u slot frame\n", it->position,
					it->frameMap->slotCount, function->frameSlots);
				return 0;
			}
			for (const LirInterval *interval = function->intervals;
					interval != NULL; interval = interval->next) {
				if (interval->spillSlot == LIR_NO_SLOT || interval->reg >= 0
						|| !intervalCovers(interval, it->position)) {
					continue;
				}
				SlotKind declared = frameMapKindAt(it->frameMap,
					(size_t) interval->spillSlot);
				if (declared != (SlotKind) interval->kind) {
					fprintf(stderr, "regalloc: slot %d holds kind %d at %d but "
						"the frame map says %d\n", interval->spillSlot,
						interval->kind, it->position, declared);
					return 0;
				}
			}
		}
	}
	return 1;
}


_Bool lirVerifyAllocation(const LirFunction *function)
{
	return verifyNoSharedRegister(function)
		&& verifyOperandsResolved(function)
		&& verifyFrameMaps(function);
}
