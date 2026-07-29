#include "jit/Jit.h"
#include "jit/MacroAssembler.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "jit/CompiledMethod.h"
#include "jit/InlineCache.h"
#include "runtime/Dictionary.h"
#include "runtime/Closure.h"
#include "runtime/Primitive.h"
#include "runtime/String.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "runtime/Collection.h"
#include "memory/Heap.h"
#include "memory/Roots.h"
#include "os/Os.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Executable memory
// ---------------------------------------------------------------------------

static uint8_t *gCodePage;
static size_t gCodeUsed;
static size_t gCodeCapacity;

#define CODE_PAGE_BYTES (256 * 1024)


uint8_t *codeSpaceAllocate(size_t bytes)
{
	bytes = (bytes + 15) & ~(size_t) 15;
	if (gCodePage == NULL || gCodeUsed + bytes > gCodeCapacity) {
		size_t size = bytes > CODE_PAGE_BYTES ? bytes : CODE_PAGE_BYTES;
		// PORT_ME(wxorx): mapped RWX. A hardened build wants W^X, which means
		// mapping RW, publishing, then flipping to RX; the publish step below
		// is where that would go, and it is separated from allocation for
		// exactly that reason.
		gCodePage = osPageAlloc(size, 1);
		ASSERT(gCodePage != NULL);
		gCodeCapacity = size;
		gCodeUsed = 0;
	}
	uint8_t *address = gCodePage + gCodeUsed;
	gCodeUsed += bytes;
	return address;
}


void codeSpacePublish(uint8_t *destination, const uint8_t *bytes, size_t size)
{
	memcpy(destination, bytes, size);
	osFlushICache(destination, size);
}


// ---------------------------------------------------------------------------
// Compiled code as a root set
// ---------------------------------------------------------------------------
//
// A CodeUnit is a malloc'd C struct that holds tagged Values (its literal
// frame), and an IcCell holds its selector as a bare RawObject* so the dispatch
// path needs no untagging. Neither lives inside a heap object, so no other root
// provider can find them, and a young collection moves precisely the objects
// they name.
//
// What that costs when it is missing is worth writing down, because it is not
// obvious from a crash: the selector pointer keeps naming the corpse in the
// evacuated semispace, selector lookup is IDENTITY on an interned Symbol, and
// the corpse is not the interned one any more. So every send from that site
// answers doesNotUnderstand for a method that plainly exists, starting at the
// first collection and never before it.

// EVERY COMPILED UNIT, IN ONE STRUCTURE: an array kept sorted by entry address.
//
// It answers two different questions, and the second is why this is an array and
// not the linked list it replaces. The root walk and the cache flush ITERATE it,
// which a list did fine. jitCodeContaining SEARCHES it, and that one is called
// by compiledFrameEnter, which the send path runs on EVERY message: a linear
// scan there is O(methods compiled) per send.
//
// MEASURED, because the comment that used to sit on the scan said it "runs once
// per frame per collection, not per send" and that had stopped being true: at
// the time this was written it was 42.75% of all cycles in Richards, the largest
// single item in the profile by a factor of five. The shape is what made it easy
// to miss -- the cost is proportional to how much of the kernel has been
// compiled, so it is invisible in a small test and dominant in a real program,
// which is also why the test suite cost more than the benchmarks did.
//
// ONE structure and not two, deliberately. An index kept ALONGSIDE the list
// would be a second registration that a future caller can forget, and the miss
// is silent in the worst way: jitCodeContaining answering NULL for a live frame
// ends that segment of the root walk early, so the collector evacuates objects
// a running frame still holds. Registration has one place because there is one
// place to register in.
static NativeCode **gCompiledCode;
static size_t gCompiledCount;
static size_t gCompiledCapacity;
static CodeUnit *gRegisteredUnits;


// The number of entries whose code starts at or below `pointer`. Both the
// search and the insertion point come from this, because the ranges are
// disjoint: the only entry that can contain an address is the last one that
// starts at or below it.
static size_t compiledCodeUpperBound(const uint8_t *pointer)
{
	size_t low = 0;
	size_t high = gCompiledCount;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (gCompiledCode[middle]->entry <= pointer) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low;
}


// Not static: the SSA backend produces NativeCode too, and there is ONE place
// to register in for the reason written at gCompiledCode above -- a second
// registration a caller can forget is a live frame the collector cannot find.
void compiledCodeRegister(NativeCode *code)
{
	// A zero-length range would let two entries share a start address, and then
	// "the last one that starts at or below" stops naming one entry.
	ASSERT(code->size > 0);
	if (gCompiledCount == gCompiledCapacity) {
		size_t capacity = gCompiledCapacity == 0 ? 64 : gCompiledCapacity * 2;
		NativeCode **grown = realloc(gCompiledCode, capacity * sizeof(*grown));
		if (grown == NULL) {
			FAIL(); // clean error: silence here is a root walk that stops early
		}
		gCompiledCode = grown;
		gCompiledCapacity = capacity;
	}
	size_t index = compiledCodeUpperBound(code->entry);
	// DISJOINT AND SORTED is what the search depends on, and these are ASSERTs
	// rather than tests because the failure mode is silence: an overlapping range
	// makes the search answer the WRONG method for a return address, and the
	// wrong method's frame map is what the collector then walks.
	ASSERT(index == 0 || gCompiledCode[index - 1]->entry
		+ gCompiledCode[index - 1]->size <= code->entry);
	ASSERT(index == gCompiledCount
		|| code->entry + code->size <= gCompiledCode[index]->entry);
	memmove(&gCompiledCode[index + 1], &gCompiledCode[index],
		(gCompiledCount - index) * sizeof(*gCompiledCode));
	gCompiledCode[index] = code;
	gCompiledCount++;
}


static void compiledCodeUnregister(NativeCode *code)
{
	size_t index = compiledCodeUpperBound(code->entry);
	if (index == 0 || gCompiledCode[index - 1] != code) {
		return;
	}
	index--;
	memmove(&gCompiledCode[index], &gCompiledCode[index + 1],
		(gCompiledCount - index - 1) * sizeof(*gCompiledCode));
	gCompiledCount--;
}


// A unit is registered when it is BUILT, not when it is compiled, and the
// difference is a block: the front end builds a block's unit while compiling the
// enclosing method, and that unit does not run until somebody sends the closure
// `value`. Everything in between is ordinary program execution, collections
// included, and the block's literal frame is reachable from nowhere else.
void jitRegisterUnit(CodeUnit *unit)
{
	if (unit == NULL || unit->registered) {
		return;
	}
	unit->registered = 1;
	unit->nextUnit = gRegisteredUnits;
	gRegisteredUnits = unit;
}


static void visitUnitField(RootVisitor visit, void *ctx, Value *field)
{
	// A zeroed field is tagInt(0), not a pointer, so a unit registered before it
	// is fully populated is walkable at every moment in between.
	if (valueTypeOf(*field, VALUE_POINTER)) {
		visit(ctx, field);
	}
}


void rootsVisitCompiledCode(RootVisitor visit, void *ctx)
{
	for (CodeUnit *unit = gRegisteredUnits; unit != NULL; unit = unit->nextUnit) {
		visitUnitField(visit, ctx, &unit->literals);
		visitUnitField(visit, ctx, &unit->blocks);
		visitUnitField(visit, ctx, &unit->selector);
		visitUnitField(visit, ctx, &unit->ownerClass);
	}
	for (size_t index = 0; index < gCompiledCount; index++) {
		NativeCode *code = gCompiledCode[index];
		// The cells' selectors are UNTAGGED, so they are tagged for the visit
		// and written back, exactly as the class table does for its entries.
		for (uint16_t i = 0; i < code->unit->instructionCount; i++) {
			if (code->cells[i].selector == NULL) {
				continue;
			}
			Value slot = tagPtr(code->cells[i].selector);
			visit(ctx, &slot);
			code->cells[i].selector = asObject(slot);
		}
	}
}


// PENDING: nothing ever LEAVES either list. Methods are never discarded today,
// so a unit is live for as long as the process is; when redefinition arrives,
// retiring a method has to unregister its unit, and the block units it owns
// with it.


// Forget every resolved target at every send site.
//
// A cell's way holds the NativeCode a class dispatched to LAST TIME, so a site
// that has already run keeps calling a method that has just been removed or
// replaced. That is a wrong ANSWER and not a crash, and it appears only at sites
// warm enough to have a way, which is the worst possible distribution for
// noticing it.
//
// The COUNTS ARE KEPT, and so is `megamorphic`. What a removal invalidates is
// "which code does this class dispatch to", not "which classes has this site
// seen": the profile is what the optimizer learns from, and throwing it away
// would make a dev-time reload cost the tier its accumulated knowledge for no
// correctness reason.
//
// EVERY TARGET IS CLEARED, not just the way count. Zeroing wayCount alone was
// enough while the runtime was the only reader, because the next icRecord starts
// over at way 0. It stopped being enough the moment the EMITTED code grew a fast
// path: that path reads way 0's class and target without consulting wayCount, so
// a stale target would stay armed inside compiled code and keep calling the
// method that was just replaced. Same wrong ANSWER this function exists to
// prevent, reached by the half of the system that was added after the comment.
void jitFlushSendCaches(void)
{
	for (size_t index = 0; index < gCompiledCount; index++) {
		NativeCode *code = gCompiledCode[index];
		for (uint16_t i = 0; i < code->unit->instructionCount; i++) {
			code->cells[i].wayCount = 0;
			for (uint8_t way = 0; way < IC_MAX_WAYS; way++) {
				code->cells[i].ways[way].target = NULL;
			}
		}
	}
}


// The same question answered the slow way: every range tested, order and
// disjointness assumed nowhere. It exists to be an INDEPENDENT oracle for the
// search below, which is why it walks the array rather than sharing any part of
// it.
static NativeCode *compiledCodeContainingLinear(const uint8_t *pointer)
{
	for (size_t index = 0; index < gCompiledCount; index++) {
		NativeCode *code = gCompiledCode[index];
		if (pointer >= code->entry && pointer < code->entry + code->size) {
			return code;
		}
	}
	return NULL;
}


// ST_JIT_CODE_MAP_CHECK=1 answers every query BOTH ways and compares.
//
// The knob exists because the invariant the search rests on is one no test can
// see. If the array ever went out of order, or two ranges overlapped, the search
// would answer the WRONG method for a return address rather than no method at
// all, and the wrong method's frame map is what the collector would then walk:
// silent corruption, arbitrarily far from the cause. Read once and cached; unset
// is the default and costs one predictable branch.
static _Bool codeMapCheckEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_JIT_CODE_MAP_CHECK") != NULL;
	}
	return enabled != 0;
}


NativeCode *jitCodeContaining(const void *address)
{
	const uint8_t *pointer = address;
	// The ranges are disjoint and sorted, so the only entry that can contain
	// `pointer` is the last one that starts at or below it. One comparison after
	// the search, and a miss is the same NULL the scan answered: an address in
	// no compiled method is the boundary where C called in, which is a normal
	// answer and not an error.
	size_t index = compiledCodeUpperBound(pointer);
	NativeCode *found = NULL;
	if (index > 0) {
		NativeCode *code = gCompiledCode[index - 1];
		if (pointer < code->entry + code->size) {
			found = code;
		}
	}
	if (codeMapCheckEnabled()) {
		ASSERT(found == compiledCodeContainingLinear(pointer));
	}
	return found;
}


const FrameMap *jitFrameMapAt(const NativeCode *code, const void *returnAddress)
{
	// A tier-1 method has ONE map and it covers everything, so the offset is not
	// consulted yet. It is taken and checked anyway: when tier 2 brings a map per
	// safepoint this becomes a search, and every caller already passes what the
	// search will need.
	ASSERT(returnAddress >= (const void *) code->entry
		&& returnAddress < (const void *) (code->entry + code->size));
	return code->frameMap;
}


// ---------------------------------------------------------------------------
// Compiled frames as a root set
// ---------------------------------------------------------------------------
//
// EVERY SLOT OF A TIER-1 FRAME HOLDS A TAGGED VALUE. That is not an accident
// and it is what makes this function short: the template compiler never puts a
// raw double or a raw integer in a frame slot, and the prologue nils every slot
// it does not receive an argument in. So the frame map for tier 1 is uniform --
// "slots 0 to frameSlots-1, all pointers" -- and no per-safepoint map is needed
// at this tier at all.
//
// That changes at tier 2, where the SSA backend will keep raw values in slots
// and the map has to say which is which (memory/Roots.h, requirement R1 of ADR
// 0003). The uniformity is a property of the TEMPLATE compiler, so it is stated
// here rather than assumed elsewhere.
//
// The walk goes newest to oldest along the saved-frame-pointer chain and stops
// when a return address is not inside any compiled method, which is exactly the
// boundary where C called in.

void rootsVisitNativeFrames(struct Thread *thread, RootVisitor visit, void *ctx)
{
	// Every SEGMENT of compiled frames, newest first. One pass per segment,
	// because C frames sit between them and the saved-frame-pointer chain runs
	// straight through those without any way to tell.
	for (const CompiledFrameGuard *guard = thread->compiledFrames; guard != NULL;
			guard = guard->previous) {
		uint8_t *frame = guard->frame;
		NativeCode *code = guard->code;
		const void *at = guard->returnAddress;
		while (frame != NULL && code != NULL) {
			// A frame the walk reached and cannot describe is a VM bug, not a
			// boundary: the boundary is a return address in NO compiled method,
			// and that ends the loop below. Reaching here with no map would mean
			// scanning by guesswork, which is the repair ADR 0003 R2 forbids.
			const FrameMap *map = jitFrameMapAt(code, at);
			ASSERT(map != NULL);
			// Slot i is at frame - 8*(i+1). The same expression the backend
			// emits, and the reason the frame layout is a declared contract
			// (jit/Jit.h).
			for (uint16_t i = 0; i < code->frameSlots; i++) {
				// The MAP decides whether this is a tagged Value at all. A raw
				// double whose bit pattern happens to satisfy the tag test is
				// exactly the accident R1 exists to prevent, and no amount of
				// looking at the contents can tell it from a pointer.
				if (frameMapKindAt(map, i) != SLOT_POINTER) {
					continue;
				}
				Value *slot = (Value *) (frame - (size_t) (i + 1) * sizeof(Value));
				// Tagged does not mean POINTER: a slot holding a SmallInteger is
				// described correctly and simply has nothing to visit.
				if (valueTypeOf(*slot, VALUE_POINTER)) {
					visit(ctx, slot);
				}
			}
			// [frame + 8] is the return address INTO THE CALLER, and [frame + 0]
			// is the caller's frame pointer. A return address that is in no
			// compiled method IS the boundary where C called in, and ends this
			// segment.
			void *returnAddress = *(void **) (frame + sizeof(Value));
			uint8_t *parent = *(uint8_t **) frame;
			code = jitCodeContaining(returnAddress);
			at = returnAddress;
			frame = parent;
		}
	}
}


void compiledFrameEnter(CompiledFrameGuard *guard, Value *slotAddress,
	uint16_t slotIndex, void *returnAddress)
{
	// slotAddress is frame - 8*(index+1), so the frame pointer follows from it.
	guard->frame = (uint8_t *) slotAddress
		+ (size_t) (slotIndex + 1) * sizeof(Value);
	guard->code = jitCodeContaining(returnAddress);
	guard->returnAddress = returnAddress;
	guard->previous = CurrentThread.compiledFrames;
	CurrentThread.compiledFrames = guard;
}


void compiledFrameLeave(const CompiledFrameGuard *guard)
{
	ASSERT(CurrentThread.compiledFrames == guard);
	CurrentThread.compiledFrames = guard->previous;
}


// Anchor a frame whose caller IS compiled code. That is what the three runtime
// helpers the template emits calls to have in common, and what a primitive does
// NOT have: PRIMITIVE_ALLOCATES is legitimately reached from C as well, so it
// anchors through compiledFrameEnter directly and a NULL there is an answer.
//
// The ASSERT enforces the one failure the registration design above ARGUES away
// rather than checks: a NativeCode that runs but never entered the array.
// jitCodeContaining would answer NULL, this frame would be anchored with no
// code, and rootsVisitNativeFrames ends the segment exactly there -- so the
// collector evacuates objects a running frame still holds, arbitrarily far from
// the cause. A named abort at the first send beats that.
static void compiledFrameEnterFromCompiled(CompiledFrameGuard *guard,
	Value *slotAddress, uint16_t slotIndex, void *returnAddress)
{
	compiledFrameEnter(guard, slotAddress, slotIndex, returnAddress);
	ASSERT(guard->code != NULL);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
//
// The whole send path, with no inline cache yet: find the receiver's class by
// INDEX, walk the superclass chain looking the selector up in each method
// dictionary, compile the method if this is its first call, and enter it.
//
// Deliberately the slow, obvious version first. The inline cache is a guard
// placed in front of this, not a replacement for it: the miss path has to land
// somewhere, and that somewhere is here.

Value jitDispatch(void *cellPointer, Value *receiverSlot, uint64_t argc);


static RawCompiledMethod *lookupMethod(RawClass *class, RawObject *selector)
{
	HandleScope scope;
	openHandleScope(&scope);
	Class *classHandle = scopeHandle(class);
	String *selectorHandle = scopeHandle(selector);

	while (classHandle->raw != NULL) {
		Value dictionaryValue = classHandle->raw->methodDictionary;
		if (valueTypeOf(dictionaryValue, VALUE_POINTER)) {
			Dictionary *dictionary = scopeHandle(asObject(dictionaryValue));
			Association *found = symbolDictAssocAt(dictionary, selectorHandle);
			if (found != NULL) {
				RawCompiledMethod *method =
					(RawCompiledMethod *) asObject(found->raw->value);
				closeHandleScope(&scope, NULL);
				return method;
			}
		}
		RawClass *super = rawClassSuperclass(classHandle->raw);
		if (super == NULL) {
			break;
		}
		classHandle = scopeHandle((RawObject *) super);
	}
	closeHandleScope(&scope, NULL);
	return NULL;
}


// Message, as packages/Core/src/Message.st declares it: `| selector arguments |`
// and nothing else. Named here so the two slots this file writes are written
// down where they are used rather than being indices 0 and 1.
typedef struct {
	OBJECT_HEADER;
	Value selector;
	Value arguments;
} RawMessageObject;


// The message nobody understood, handed BACK TO SMALLTALK.
//
// `Object>>doesNotUnderstand:` exists in the kernel and signals a
// MessageNotUnderstood, which is an ordinary exception an `on:do:` can catch.
// Until this was wired, the VM printed the selector and called abort(), so a
// program could not catch a missing method AT ALL -- and a suite full of tests
// that deliberately send an unknown selector had no way to run.
//
// Answers 0 when the send could not be made, and then the caller prints and
// aborts exactly as before. That happens in two cases and both are real: the
// BUILT-IN kernel has no Message class, and a receiver that does not understand
// `doesNotUnderstand:` itself must not be asked twice.
static _Bool sendDoesNotUnderstand(IcCell *cell, Value *receiverSlot,
	uint64_t argc, Value *answer)
{
	// The recursion guard, and it has to be first: asking a receiver that does
	// not understand `doesNotUnderstand:` to understand `doesNotUnderstand:`
	// would come straight back here, forever.
	if (rawStringEqualsBytes((RawString *) cell->selector, "doesNotUnderstand:",
			sizeof "doesNotUnderstand:" - 1)) {
		return 0;
	}
	Class *messageClass = getClass("Message");
	if (messageClass == NULL) {
		return 0;
	}

	HandleScope scope;
	openHandleScope(&scope);
	// The ARGUMENTS first, and read out of the frame AFTER the Array exists:
	// allocating can collect, and the collector updates the frame slots it walks
	// but not a copy taken before.
	Array *arguments = newArray(argc);
	for (uint64_t i = 0; i < argc; i++) {
		// A fresh Array is young, so a young-to-anything store needs no barrier;
		// the barrier exists for an OLD object learning about a young one.
		arguments->raw->vars[i] = receiverSlot[-(intptr_t) (i + 1)];
	}
	Object *message = newObject(messageClass, 0);
	RawMessageObject *raw = (RawMessageObject *) message->raw;
	// cell->selector is re-read here rather than saved above: it is a bare
	// pointer that rootsVisitCompiledCode updates, so the allocations just made
	// may have moved what it names.
	rawObjectStorePtr((RawObject *) raw, &raw->selector, cell->selector);
	rawObjectStorePtr((RawObject *) raw, &raw->arguments,
		(RawObject *) arguments->raw);
	Value messageValue = objectTagged(message);
	Value receiver = *receiverSlot; // re-read: the allocations may have moved it
	closeHandleScope(&scope, NULL);

	_Bool understood = 0;
	Value result = jitSend(receiver, "doesNotUnderstand:", 1, &messageValue,
		&understood);
	if (!understood) {
		return 0;
	}
	*answer = result;
	return 1;
}


// The tier-2 dry run (jit/Tier2DryRun.c), reached through a WEAK no-op for the
// same reason rootsVisitNativeFrames is: gate levels 3, 8 and 9 hand-link this
// file with half a dozen others and no optimizer at all, and a hard reference to
// SsaBuild from here would make those levels stop linking. That is not
// hypothetical -- it is what happened the first time this was wired.
__attribute__((weak))
void tier2DryRun(CodeUnit *unit)
{
	(void) unit;
}


// ST_IC_STATS=1 reports, at exit, how many sends the inline cache served itself
// and how many reached the runtime.
//
// It exists because the two things worth doubting about a fast path are exactly
// the two a benchmark cannot tell apart. FIRST, is it taken at all: a cache that
// always misses is invisible, every test passes and the clock barely moves.
// SECOND, and this project has a scar from it: is the PROFILE still true? The
// previous VM's arithmetic fast path bypassed its cache when it hit, so a hot
// site's profile described only the executions that had FAILED -- inverted, not
// incomplete (ADR 0004). The check for that is not a number but a COMPARISON:
// run the same deterministic program with ST_NO_INLINE_CACHE=1 and the send
// totals must come out IDENTICAL, because the emitted hit counts exactly what
// icRecord counts.
static uint64_t gDispatchMisses;

// WHERE THE MISSES ARE, which is the only question worth asking next: a
// monomorphic cache tests one class, so every send to a site's second class is a
// miss by construction. The breakdown says whether that is worth a second way or
// whether the misses are somewhere else entirely, and it is read out of the
// cells rather than counted at miss time because the cells already hold it.
static void icStatsReport(void)
{
	uint64_t sends = 0;
	uint64_t wayCounts = 0;
	uint64_t inWay0 = 0;       // what the emitted cache can serve
	uint64_t inWay1 = 0;       // what a SECOND way in the emitted cache would serve
	uint64_t inOtherWays = 0;  // what it still would not
	uint64_t megamorphicSends = 0;
	uint64_t unarmedSends = 0; // way 0 exists but nothing ever resolved into it
	uint64_t sites = 0, armedSites = 0, polySites = 0, megaSites = 0;
	for (size_t index = 0; index < gCompiledCount; index++) {
		NativeCode *code = gCompiledCode[index];
		for (uint16_t i = 0; i < code->unit->instructionCount; i++) {
			const IcCell *cell = &code->cells[i];
			sends += cell->sends;
			if (cell->sends == 0) {
				continue;
			}
			sites++;
			if (cell->megamorphic) {
				megaSites++;
				megamorphicSends += cell->sends;
			} else if (cell->ways[0].target == NULL) {
				unarmedSends += cell->sends;
			} else {
				armedSites++;
			}
			if (cell->wayCount > 1) {
				polySites++;
			}
			for (uint8_t way = 0; way < cell->wayCount; way++) {
				wayCounts += cell->ways[way].count;
				if (way == 0) {
					inWay0 += cell->ways[way].count;
				} else if (way == 1) {
					inWay1 += cell->ways[way].count;
				} else {
					inOtherWays += cell->ways[way].count;
				}
			}
		}
	}
	fprintf(stderr, "ic: sends=%llu ways=%llu misses=%llu hits=%llu\n",
		(unsigned long long) sends, (unsigned long long) wayCounts,
		(unsigned long long) gDispatchMisses,
		(unsigned long long) (sends - gDispatchMisses));
	fprintf(stderr, "ic: way0=%llu way1=%llu ways2plus=%llu megamorphic=%llu unarmed=%llu\n",
		(unsigned long long) inWay0, (unsigned long long) inWay1,
		(unsigned long long) inOtherWays,
		(unsigned long long) megamorphicSends, (unsigned long long) unarmedSends);
	fprintf(stderr, "ic: sites=%llu armed=%llu polymorphic=%llu megamorphic=%llu\n",
		(unsigned long long) sites, (unsigned long long) armedSites,
		(unsigned long long) polySites, (unsigned long long) megaSites);
	fflush(NULL);
}


// The counter is UNCONDITIONAL and only the report is gated. An env test per
// dispatch would put a branch on the hot miss path to save one increment, which
// is the wrong trade in both directions.
static void icStatsCountMiss(void)
{
	static _Bool armed;
	gDispatchMisses++;
	if (!armed) {
		armed = 1;
		if (getenv("ST_IC_STATS") != NULL) {
			atexit(icStatsReport);
		}
	}
}


// May compiled code call this method DIRECTLY, through a send site's inline
// cache, instead of coming through one of the C entry points at the bottom of
// this file?
//
// Only when entering it costs nothing but the call, and there is exactly one
// thing today that costs more. A method that could be the HOME of a non-local
// return needs an unwind record, and the record's jump destination has to be
// taken by setjmp IN THE FRAME THAT RESUMES -- so it cannot be pushed by the
// callee's own prologue, and generated code cannot take a setjmp at all. Enter
// such a method directly and a `^` inside one of its blocks finds no record
// carrying its token, which the token scheme then reports as a return from an
// activation that has already returned. That is not a hypothesis: it is what the
// gate answered the first time this fast path was wired without this test.
//
// ONE PREDICATE, and the only place that arms a cache reads it, so an obligation
// added to ENTER_COMPILED has one place here to be added too.
static _Bool jitMayEnterDirectly(const NativeCode *code)
{
	return !code->unit->couldBeHome;
}


// The body of both send paths. `lookupStart` is the class the method search
// begins at, and it is the ONLY thing that differs between an ordinary send and
// a super send: everything else, the profile included, is the same site
// machinery. `returnAddress` has to be taken in the function compiled code
// called, which is why it is a parameter here.
static Value dispatchFrom(IcCell *cell, Value *receiverSlot, uint64_t packed,
	RawClass *lookupStart, void *returnAddress)
{
	Value receiver = *receiverSlot;
	uint64_t argc = packed & 0xFFFFFFFFu;

	// Anchor the caller's frame before doing anything that can allocate, so a
	// collection from in here can walk the compiled frames underneath.
	CompiledFrameGuard guard;
	compiledFrameEnterFromCompiled(&guard, receiverSlot, (uint16_t) (packed >> 32),
		returnAddress);

	// PROFILE FIRST, before anything can fail. The receiver's class and, when
	// there is one, the first ARGUMENT's class: that second one is what lets
	// the optimizer choose between integer and floating-point arithmetic
	// without guessing, and it is the field easiest to forget.
	icStatsCountMiss();
	uint32_t receiverClass = classIndexOfValue(receiver);
	uint32_t argumentClass = argc > 0
		? classIndexOfValue(receiverSlot[-1]) : CLASS_INDEX_INVALID;
	IcWay *way = icRecord(cell, receiverClass, argumentClass);

	RawCompiledMethod *method = lookupStart == NULL
		? NULL : lookupMethod(lookupStart, cell->selector);
	if (method == NULL) {
		// TO SMALLTALK FIRST. `Object>>doesNotUnderstand:` signals a catchable
		// MessageNotUnderstood, which is what makes a missing method a condition
		// a program can handle rather than the end of the process.
		Value answer;
		if (sendDoesNotUnderstand(cell, receiverSlot, argc, &answer)) {
			compiledFrameLeave(&guard);
			return answer;
		}
		// Nothing could take it: print loudly and stop. Failing loudly beats
		// returning nil, which would turn a missing method into a wrong answer
		// somewhere else.
		// EVERY PART OF THIS GOES TO STDERR, and the selector is why: printing it
		// on stdout put it in a block-buffered stream that abort() never flushes,
		// so the message arrived as "doesNotUnderstand: " with nothing after it.
		//
		// The RECEIVER'S CLASS is named too. Without it the message says what was
		// not understood and leaves out who did not understand it, which is the
		// half that says where to look.
		fprintf(stderr, "doesNotUnderstand: ");
		fprintRawString(stderr, (RawString *) cell->selector);
		RawClass *receiverClassObject = classOf(receiver);
		Value className = receiverClassObject == NULL
			? 0 : receiverClassObject->name;
		fprintf(stderr, " (receiver is ");
		if (valueTypeOf(className, VALUE_POINTER)) {
			fprintf(stderr, "a ");
			fprintRawString(stderr, (RawString *) asObject(className));
		} else {
			fprintf(stderr, "of class %u", receiverClass);
		}
		fprintf(stderr, ")");
		// Arguments that are Symbols or Strings are printed, and nothing else is.
		// A bodyless primitive method fails as `self primitiveFailed: #TheName`
		// (compiler/Compile.c), so without this the message names the receiver's
		// class and omits WHICH primitive was being attempted, which for a class
		// like Float is a dozen candidates. Text-shaped arguments are the ones
		// worth printing and the only ones that are safe to print from here.
		for (uint64_t i = 0; i < argc; i++) {
			Value argument = receiverSlot[-(intptr_t) (i + 1)];
			if (!valueTypeOf(argument, VALUE_POINTER)) {
				continue;
			}
			RawClass *argumentClassObject = classOf(argument);
			if (argumentClassObject != Handles.String.raw
					&& argumentClassObject != Handles.Symbol.raw) {
				continue;
			}
			fprintf(stderr, " (argument %llu is ", (unsigned long long) (i + 1));
			fprintRawString(stderr, (RawString *) asObject(argument));
			fprintf(stderr, ")");
		}
		fprintf(stderr, "\n");
		fflush(NULL);
		abort();
	}
	if (method->native == NULL) {
		Opcode unsupported;
		method->native = jitCompile(method->unit, &unsupported);
		if (method->native == NULL) {
			fprintf(stderr, "jit: unsupported opcode %s\n", opcodeName(unsupported));
			abort();
		}
	}
	NativeCode *code = method->native;
	// ARM THE INLINE CACHE, but only for a target whose calling convention is the
	// one the SITE emitted for, and only for one compiled code may enter at all.
	//
	// The site decided narrow-or-wide from its own argument count, because a send
	// of n arguments can only reach a method of arity n and the convention is a
	// function of arity and ABI alone. Installing a target that disagreed would
	// have compiled code hand over a pointer where the callee reads registers, or
	// the reverse, and the failure is a callee reading arguments nobody wrote --
	// no crash at the call, a wrong answer later.
	//
	// It cannot happen. It is CHECKED anyway, and the check is the condition
	// rather than an assertion, because a build with assertions off would
	// otherwise arm the site: leaving the target null is always safe, and only
	// costs the site its fast path.
	if (way != NULL && jitMayEnterDirectly(code)
			&& code->unit->argumentCount == argc) {
		ASSERT(code->wide
			== maWideForArity(maHostBackend()->abi, (uint16_t) argc));
		way->target = code;
		// The fast path reads way 0 and only way 0, so the hottest class has to
		// BE way 0 or the cache tests a class it rarely sees.
		icPromoteHottest(cell);
	}
	// Arguments are at DESCENDING addresses from the receiver's slot, because
	// consecutive registers are consecutive slots and slots grow down.
	//
	// Re-read from the FRAME and not from the local above: the frame slot is a
	// root the collector updates, and a bare Value held in a C local across the
	// lookup would not be. Nothing between here and there collects today, and
	// this is what keeps that from being a requirement.
	receiver = *receiverSlot;
	Value answer;
	if (code->wide) {
		// The arguments are ALREADY laid out the way a wide callee reads them:
		// this frame's receiver slot with the arguments descending from it. So
		// the wide path is the cheap one here, and it is the narrow cases below
		// that pay to take frame slots apart into positional C arguments.
		answer = jitCallWide(code, receiverSlot);
	} else {
		switch (argc) {
		case 0:
			answer = jitCall0(code, receiver);
			break;
		case 1:
			answer = jitCall1(code, receiver, receiverSlot[-1]);
			break;
		case 2:
			answer = jitCall2(code, receiver, receiverSlot[-1], receiverSlot[-2]);
			break;
		case 3:
			answer = jitCall3(code, receiver, receiverSlot[-1], receiverSlot[-2],
				receiverSlot[-3]);
			break;
		case 4:
			answer = jitCall4(code, receiver, receiverSlot[-1], receiverSlot[-2],
				receiverSlot[-3], receiverSlot[-4]);
			break;
		default:
			answer = jitCall5(code, receiver, receiverSlot[-1], receiverSlot[-2],
				receiverSlot[-3], receiverSlot[-4], receiverSlot[-5]);
			break;
		}
	}
	compiledFrameLeave(&guard);
	return answer;
}


Value jitDispatch(void *cellPointer, Value *receiverSlot, uint64_t packed)
{
	return dispatchFrom(cellPointer, receiverSlot, packed, classOf(*receiverSlot),
		__builtin_return_address(0));
}


// `super`, and the whole content of the keyword is the one argument that differs
// from the line above: the search starts where the COMPILER said, at the class
// ABOVE the one that defined the running method, and not at anything the
// receiver names. That is what makes `super foo` inside `Foo>>foo` finite, and
// it is why the start cannot be derived at runtime from the receiver, which may
// be an instance of a subclass three levels down.
//
// The receiver is unchanged: super is not another object, it is another place to
// start looking.
Value jitDispatchSuper(void *cellPointer, Value *receiverSlot, uint64_t packed)
{
	IcCell *cell = cellPointer;
	// A super send in a class with no superclass has nowhere to look, and NULL
	// takes it to doesNotUnderstand. Falling back to the receiver's class would
	// find the running method again and recurse until the stack ran out.
	RawClass *start = cell->lookupStart == CLASS_INDEX_INVALID ? NULL
		: (RawClass *) classTableAt(&CurrentThread.heap->classes, cell->lookupStart);
	return dispatchFrom(cell, receiverSlot, packed, start,
		__builtin_return_address(0));
}


// A runtime helper is handed the ADDRESS of one frame slot plus, packed into an
// immediate the call sequence was already materialising, WHICH slot that was and
// whatever else it needs. The slot index is what turns the address back into a
// frame pointer, which is how the helper reaches the rest of the frame and how a
// collection under it finds the compiled frames beneath.
#define PACKED_LOW(packed) ((uint16_t) ((packed) & 0xFFFF))
#define PACKED_MID(packed) ((uint16_t) (((packed) >> 16) & 0xFFFF))
#define PACKED_SLOT(packed) ((uint16_t) (((packed) >> 32) & 0xFFFF))


// Send a unary message from C.
//
// It exists for the bootstrap: a class-side `initialize` has to run before
// anything the class touches works, and until an image exists there is no
// Smalltalk-level place to run it from. It goes through the same lookup and the
// same entry as every other send, so a method reached this way is compiled,
// cached and profiled exactly like one reached from bytecode.
Value jitSend(Value receiver, const char *selector, uint64_t argc,
	const Value *argv, _Bool *understood)
{
	ASSERT(argc <= 2); // what the callers in this file need; widen when one does
	HandleScope scope;
	openHandleScope(&scope);
	// EVERY operand goes in a handle, arguments included: looking the selector up
	// interns a Symbol, which allocates, and an argument held as a bare Value
	// across that is a corpse by the time the call is made.
	Object *held = valueTypeOf(receiver, VALUE_POINTER)
		? (Object *) scopeHandle(asObject(receiver)) : NULL;
	Object *heldArgs[2] = { NULL, NULL };
	Value rawArgs[2] = { 0, 0 };
	for (uint64_t i = 0; i < argc; i++) {
		if (valueTypeOf(argv[i], VALUE_POINTER)) {
			heldArgs[i] = (Object *) scopeHandle(asObject(argv[i]));
		} else {
			rawArgs[i] = argv[i];
		}
	}
	String *name = asSymbol(stringFromC(selector));

	RawCompiledMethod *method = lookupMethod(classOf(
		held != NULL ? tagPtr(held->raw) : receiver), (RawObject *) name->raw);
	if (method == NULL) {
		if (understood != NULL) {
			*understood = 0;
		}
		closeHandleScope(&scope, NULL);
		return tagPtr(Handles.nil.raw);
	}
	if (understood != NULL) {
		*understood = 1;
	}
	if (method->native == NULL) {
		Opcode unsupported;
		method->native = jitCompile(method->unit, &unsupported);
		if (method->native == NULL) {
			fprintf(stderr, "jit: %s uses %s, which the tier does not implement\n",
				selector, opcodeName(unsupported));
			fflush(NULL);
			closeHandleScope(&scope, NULL);
			return tagPtr(Handles.nil.raw);
		}
	}
	Value self = held != NULL ? tagPtr(held->raw) : receiver;
	Value a0 = heldArgs[0] != NULL ? tagPtr(heldArgs[0]->raw) : rawArgs[0];
	Value a1 = heldArgs[1] != NULL ? tagPtr(heldArgs[1]->raw) : rawArgs[1];
	Value answer;
	switch (argc) {
	case 0: answer = jitCall0(method->native, self); break;
	case 1: answer = jitCall1(method->native, self, a0); break;
	default: answer = jitCall2(method->native, self, a0, a1); break;
	}
	closeHandleScope(&scope, NULL);
	return answer;
}


Value jitSendUnary(Value receiver, const char *selector, _Bool *understood)
{
	return jitSend(receiver, selector, 0, NULL, understood);
}


// ---------------------------------------------------------------------------
// Non-local return (ADR 0008)
// ---------------------------------------------------------------------------
//
// `^` inside a block returns from the method the block was WRITTEN in, however
// many activations are stacked in between. Between the block and its home there
// are compiled frames AND C frames, alternating, and the C frames are what rules
// out simply popping compiled frames.
//
// The mechanism, and what each half buys:
//
//   THE TOKEN answers "which activation", and it is a counter rather than a
//   frame address because addresses are reused: a block that outlives its home
//   must find NOTHING, and a stale address would eventually find a stranger's
//   frame sitting in the same place. The closure carries the token of the
//   activation it was born in.
//
//   THE RECORD answers "how to get there". It lives on the C frame that ENTERED
//   the home activation, so the chain unwinds itself as those frames return, and
//   it holds the jump destination plus the thread state a jump would otherwise
//   leave behind.
//
// WHO PAYS. Only a method that actually contains a `^` inside a block gets a
// record, and the front end decides that at compile time (CodeUnit.couldBeHome).
// A program that never writes one runs exactly as it did before: no token is
// minted, no record is pushed, and nothing is emitted at any send. That is why
// the cost sits here and not as a check after every send, which is the other way
// to build this and would tax the hottest path in the system for a feature most
// sends never use.

// The fields common to every kind. The Values start as nil rather than as the
// allocator's zero because the record is a GC root from the moment it is on the
// chain, and a collection may look at it before it is fully filled.
static void unwindPushCommon(UnwindRecord *record, UnwindKind kind)
{
	record->kind = (uint8_t) kind;
	record->disabled = 0;
	record->token = 0;
	record->exceptionClass = tagPtr(Handles.nil.raw);
	record->handlerBlock = tagPtr(Handles.nil.raw);
	record->cleanupBlock = tagPtr(Handles.nil.raw);
	record->answer = tagPtr(Handles.nil.raw);
	record->savedFrames = CurrentThread.compiledFrames;
	record->savedScopes = CurrentThread.handleScopes;
	record->savedHomeToken = CurrentThread.homeToken;
	record->previous = CurrentThread.unwinds;
	CurrentThread.unwinds = record;
}


// Make the activation about to be entered findable by a non-local return.
static void unwindPush(UnwindRecord *record)
{
	unwindPushCommon(record, UNWIND_HOME);
	record->token = ++CurrentThread.nextHomeToken;
	CurrentThread.homeToken = record->token;
}


void unwindPushHandler(UnwindRecord *record, Value exceptionClass,
	Value handlerBlock)
{
	unwindPushCommon(record, UNWIND_HANDLER);
	record->exceptionClass = exceptionClass;
	record->handlerBlock = handlerBlock;
}


void unwindPushCleanup(UnwindRecord *record, Value cleanupBlock)
{
	unwindPushCommon(record, UNWIND_CLEANUP);
	record->cleanupBlock = cleanupBlock;
}


// The bottom of one fiber's Smalltalk execution, and the one place terminating
// that fiber can land.
//
// IT DOES NOT GO THROUGH unwindPushCommon, and the difference is the four
// tagged fields: they are the allocator's ZERO here rather than nil. An exit
// record holds no block and answers no value, so absent is the truth about them
// and the root visitor skips them; and it is pushed BEFORE anything a fiber
// runs, which for the main process is before an image may be loaded. An image
// load rewrites every well-known handle, so a nil written here at startup would
// leave this record rooting the PREVIOUS kernel's nil for the whole run.
void unwindPushExit(UnwindRecord *record)
{
	record->kind = UNWIND_EXIT;
	record->disabled = 0;
	record->token = 0;
	record->exceptionClass = tagInt(0);
	record->handlerBlock = tagInt(0);
	record->cleanupBlock = tagInt(0);
	record->answer = tagInt(0);
	record->savedFrames = CurrentThread.compiledFrames;
	record->savedScopes = CurrentThread.handleScopes;
	record->savedHomeToken = CurrentThread.homeToken;
	record->previous = CurrentThread.unwinds;
	CurrentThread.unwinds = record;
}


void unwindPop(UnwindRecord *record)
{
	ASSERT(CurrentThread.unwinds == record);
	CurrentThread.unwinds = record->previous;
	CurrentThread.homeToken = record->savedHomeToken;
}


// Arrived by a jump rather than by a return: the frames in between were skipped,
// so their entries on these chains are put back from what was saved.
//
// PENDING: the handle scopes of the skipped frames are dropped by restoring the
// head, which leaks their overflow chunks. Bounded by one unwind.
Value unwindAnswer(UnwindRecord *record)
{
	CurrentThread.unwinds = record->previous;
	CurrentThread.compiledFrames = record->savedFrames;
	CurrentThread.handleScopes = record->savedScopes;
	CurrentThread.homeToken = record->savedHomeToken;
	return record->answer;
}


// The tagged fields of every record on this thread's chain.
//
// A handler block is held across the ENTIRE evaluation of the block it
// protects, which allocates as freely as any other code, so these are ordinary
// long-lived roots and not a technicality. Without this the first collection
// inside a protected block leaves the handler pointing at a corpse, and the
// symptom is an `on:do:` that runs garbage only when the block allocated enough
// to collect.
void rootsVisitUnwindRecords(struct Thread *thread, RootVisitor visit, void *ctx)
{
	for (UnwindRecord *record = thread->unwinds; record != NULL;
			record = record->previous) {
		Value *fields[] = { &record->exceptionClass, &record->handlerBlock,
			&record->cleanupBlock, &record->answer };
		for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
			if (valueTypeOf(*fields[i], VALUE_POINTER)) {
				visit(ctx, fields[i]);
			}
		}
	}
}


// Run every `ensure:` cleanup between here and `target`, innermost first, and
// leave the chain head at `target`.
//
// Each record is UNLINKED BEFORE its block runs. A cleanup can signal, or do a
// non-local return of its own, and the unwind that follows must not find this
// same cleanup still armed and run it a second time.
static void unwindRunCleanupsTo(UnwindRecord *target)
{
	while (CurrentThread.unwinds != NULL && CurrentThread.unwinds != target) {
		UnwindRecord *record = CurrentThread.unwinds;
		CurrentThread.unwinds = record->previous;
		if (record->kind != UNWIND_CLEANUP) {
			continue;
		}
		_Bool understood = 0;
		jitSendUnary(record->cleanupBlock, "value", &understood);
	}
}


// Terminate the running fiber. Never returns.
//
// THE CLEANUPS RUN BEFORE THE JUMP AND ON THIS STACK, and that is the whole
// difference between terminating a fiber and abandoning one. An `ensure:` block
// belongs to an activation, and the only stack that activation exists on is the
// one running right now; a terminate that reclaimed the stack first would have
// nowhere left to run them. It is the third client of unwindRunCleanupsTo, and
// the other two are the non-local return and the exception unwind.
//
// WHERE IT LANDS is the only difference between the main process and any other
// fiber, and it is not decided here: main() ends the program, the spawn
// trampoline marks the fiber DONE and hands the CPU on. Both are just the frame
// that pushed the exit record.
void unwindToExit(void)
{
	UnwindRecord *bottom = CurrentThread.unwinds;
	while (bottom != NULL && bottom->kind != UNWIND_EXIT) {
		bottom = bottom->previous;
	}
	if (bottom == NULL) {
		// A fiber whose bottom was never marked. Every entry into Smalltalk
		// pushes one, so this is a VM bug and not a program error, and jumping
		// nowhere or answering would be silence in the middle of a terminate.
		fprintf(stderr, "terminate: this fiber has no exit point\n");
		fflush(NULL);
		abort();
	}
	unwindRunCleanupsTo(bottom);
	longjmp(bottom->destination, 1);
}


// ---------------------------------------------------------------------------
// Signalling
// ---------------------------------------------------------------------------
//
// The order here is the whole resumable protocol, and it is the opposite of the
// obvious one: the handler runs BEFORE anything unwinds.
//
//   1. walk outward for the first enabled handler whose class answers `handles:`;
//   2. run it ON TOP of the signaling frames, through `runHandledBy:`, which
//      answers an Association saying what to do;
//   3. `false -> value` means RESUME: answer `value` from the signal expression,
//      with nothing popped and every pending `ensure:` still armed;
//   4. `true -> value` means UNWIND: run the cleanups between here and the
//      handler, then jump to its `on:do:`, which answers `value`.
//
// Deciding first and unwinding second is what makes `resume:` expressible at
// all. Unwinding first, as a plain longjmp handler would, destroys the frames
// the resumption has to return into before anyone has said whether to resume.
Value jitSignalException(Value exception)
{
	for (UnwindRecord *record = CurrentThread.unwinds; record != NULL;
			record = record->previous) {
		if (record->kind != UNWIND_HANDLER || record->disabled) {
			continue;
		}
		_Bool understood = 0;
		Value matched = jitSend(record->exceptionClass, "handles:", 1, &exception,
			&understood);
		if (!understood || matched != tagPtr(Handles.true_.raw)) {
			continue;
		}

		// DISABLED while it runs, so an exception raised inside a handler
		// searches OUTWARD instead of re-entering the handler already dealing
		// with one. Restored on the resume path, where this activation lives on.
		record->disabled = 1;
		Value decision = jitSend(exception, "runHandledBy:", 1,
			&record->handlerBlock, &understood);
		record->disabled = 0;
		if (!understood || !valueTypeOf(decision, VALUE_POINTER)) {
			return PRIMITIVE_FAILED;
		}

		// An Association: key says whether to unwind, value is the result.
		RawAssociation *association = (RawAssociation *) asObject(decision);
		Value unwind = association->key;
		Value answer = association->value;
		if (unwind != tagPtr(Handles.true_.raw)) {
			return answer; // resume: the signal expression answers this
		}
		record->answer = answer;
		unwindRunCleanupsTo(record);
		longjmp(record->destination, 1);
	}
	// Nobody handled it. The primitive FAILS, and `Exception>>basicSignal` falls
	// through to its own `^self defaultAction`, which is where the kernel decides
	// what an unhandled exception of this class means.
	return PRIMITIVE_FAILED;
}


// RETOUTER: return `*valueSlot` from the home of the closure in slot 0. Never
// returns to its caller.
Value jitReturnOuter(void *unused, Value *valueSlot, uint64_t packed)
{
	(void) unused;
	// The value's register is what turns its slot address back into the frame
	// pointer, and slot 0 of a block's frame is the running closure.
	uint8_t *frame = (uint8_t *) valueSlot
		+ (size_t) (PACKED_SLOT(packed) + 1) * sizeof(Value);
	RawClosure *closure = (RawClosure *) asObject(((Value *) frame)[-1]);
	uint64_t token = (uint64_t) asCInt(closure->homeToken);

	for (UnwindRecord *record = CurrentThread.unwinds; record != NULL;
			record = record->previous) {
		if (record->kind != UNWIND_HOME || record->token != token) {
			continue;
		}
		record->answer = *valueSlot;
		// A non-local return is an unwind like any other, so every `ensure:`
		// between here and the home runs on the way out. This is the second of
		// the three paths that has to do it; the third is the exception unwind.
		unwindRunCleanupsTo(record);
		longjmp(record->destination, 1);
	}
	// The home activation has already returned. ADR 0008 says this RAISES rather
	// than jumping anywhere, and the token scheme is what makes it detectable at
	// all: no live record can ever carry a retired token.
	//
	// AN ORDINARY EXCEPTION, now that there are exceptions. `[^1]` kept and sent
	// `value` after its method returned is a program error a program can want to
	// catch, and until this was wired it killed the process instead: exactly the
	// state doesNotUnderstand was in before its bridge existed, and fixed the same
	// way. The abort below stays for the window where it is still the only
	// option, which is the BUILT-IN kernel, where Error does not exist.
	Class *errorClass = getClass("Error");
	if (errorClass != NULL) {
		HandleScope scope;
		openHandleScope(&scope);
		Value text = objectTagged(stringFromC(
			"non-local return from a method that has already returned"));
		_Bool understood = 0;
		// The scope stays OPEN across the send: `text` is a heap String and
		// jitSend allocates (it interns the selector) before it uses it.
		Value raised = jitSend(objectTagged(errorClass), "signal:", 1, &text,
			&understood);
		closeHandleScope(&scope, NULL);
		// `signal:` on an unhandled Error does not come back: its default action
		// terminates the process. Arriving here means a handler RESUMED it, and
		// the value it resumed with is the only sensible thing for the abandoned
		// `^` to answer.
		if (understood) {
			return raised;
		}
	}
	fprintf(stderr, "non-local return from a method that has already returned\n");
	fflush(NULL);
	abort();
}


// Store into a global's Association. `valueSlot` points at the frame slot
// holding the value; `literalIndex` says which literal is the Association.
Value jitStoreGlobal(void *unitPointer, Value *valueSlot,
	uint64_t literalIndex)
{
	CodeUnit *unit = unitPointer;
	RawArray *literals = (RawArray *) asObject(unit->literals);
	RawObject *association = asObject(literals->vars[literalIndex]);
	// Field 1 of an Association is its value; through the barrier, because the
	// Association is reachable from a namespace that has long since been
	// promoted, and what is being stored into it was very likely allocated a
	// moment ago.
	rawObjectStoreValue(association, &((Value *) association->body)[1], *valueSlot);
	return *valueSlot;
}


// ---------------------------------------------------------------------------
// Closures (ADR 0008)
// ---------------------------------------------------------------------------
//
// All three of these ALLOCATE, so all three anchor the calling frame. They take
// the frame slot they were handed plus, in the packed integer, WHICH slot that
// was, which is what turns the address back into a frame pointer.
//
// The packing is two or three 16-bit fields in an immediate the call sequence
// was already materialising, so carrying them costs nothing.



// Build a closure over `blocks[index]`, capturing the values in the `count`
// registers starting at the base. Consecutive registers are consecutive slots
// at DESCENDING addresses, so capture i is baseSlot[-i].
Value jitMakeClosure(void *unitPointer, Value *baseSlot, uint64_t packed)
{
	CompiledFrameGuard guard;
	compiledFrameEnterFromCompiled(&guard, baseSlot, PACKED_SLOT(packed),
		__builtin_return_address(0));

	CodeUnit *unit = unitPointer;
	uint16_t index = PACKED_LOW(packed);
	uint16_t count = PACKED_MID(packed);

	// WHERE the home token comes from, and it is not a detail: a `^` inside this
	// block returns from the method the block was WRITTEN in, so a block built
	// inside another block inherits that block's home rather than naming the
	// activation it happens to be running under. Taking the thread's current
	// token here would name whatever method called `value`, which is a different
	// method entirely and usually somebody else's.
	uint64_t homeToken;
	if (unit->isBlock) {
		// Slot 0 of a block's frame IS the running closure, and the frame pointer
		// is one slot above it.
		RawClosure *running = (RawClosure *) asObject(((Value *) guard.frame)[-1]);
		homeToken = (uint64_t) asCInt(running->homeToken);
	} else {
		homeToken = CurrentThread.homeToken;
	}

	HandleScope scope;
	openHandleScope(&scope);
	RawArray *blocks = (RawArray *) asObject(unit->blocks);
	Object *method = scopeHandle(asObject(blocks->vars[index]));
	Closure *closure = newClosure(method, count, homeToken);
	// The captures are copied AFTER the allocation and read from the frame each
	// time, because allocating the closure may have moved every one of them.
	for (uint16_t i = 0; i < count; i++) {
		rawObjectStoreValue((RawObject *) closure->raw, &closure->raw->captured[i],
			baseSlot[-(intptr_t) i]);
	}
	Value answer = objectTagged(closure);
	closeHandleScope(&scope, NULL);

	compiledFrameLeave(&guard);
	return answer;
}


Value jitMakeCell(void *unused, Value *valueSlot, uint64_t packed)
{
	(void) unused;
	CompiledFrameGuard guard;
	compiledFrameEnterFromCompiled(&guard, valueSlot, PACKED_SLOT(packed),
		__builtin_return_address(0));
	Value answer = objectTagged(newCell(*valueSlot));
	compiledFrameLeave(&guard);
	return answer;
}


// Store into ANY tagged field of any object, through the write barrier.
//
// The generalisation of jitSetCell below, and it exists because the SSA backend
// cannot tell a cell store from an instance-variable store: the SSA IR models
// both as IR_SETFIELD_T, and CELL_VALUE_FIELD is 0, which is also a perfectly
// ordinary instance-variable index. So the two are indistinguishable in the IR,
// and the only safe reading of an indistinguishable pair is the conservative
// one -- barrier both.
//
// Which is also STRICTLY MORE CORRECT than what tier 1 does. OP_SETIVAR there
// is an inline store with `PENDING: the write barrier` written at it, so an old
// object receiving a young one in an instance variable is not remembered. That
// is a real hole; it is simply not this file's to close today, and closing it
// for tier 2 costs a call that tier 1 does not pay.
//
// The packed integer carries the value's register and the object's, exactly as
// jitSetCell's does, because the same two slots have to be found.
Value jitStoreField(void *unused, Value *objectSlot, uint64_t packed)
{
	(void) unused;
	uint16_t objectRegister = PACKED_SLOT(packed);
	uint16_t valueRegister = PACKED_LOW(packed);
	uint16_t fieldIndex = PACKED_MID(packed);
	// Slot i sits at frame - 8*(i+1), so a higher register is a LOWER address.
	Value *valueSlot = objectSlot
		- ((intptr_t) valueRegister - (intptr_t) objectRegister);
	RawObject *object = asObject(*objectSlot);
	rawObjectStoreValue(object, &((Value *) object->body)[fieldIndex], *valueSlot);
	return *valueSlot;
}


// Store into a cell, through the write barrier. A cell outlives the frame that
// made it (that is the whole point of having one), so it is routinely old while
// what goes into it is young.
Value jitSetCell(void *unused, Value *cellSlot, uint64_t packed)
{
	(void) unused;
	uint16_t cellRegister = PACKED_SLOT(packed);
	uint16_t valueRegister = PACKED_LOW(packed);
	// Slot i sits at frame - 8*(i+1), so a higher register is a LOWER address.
	Value *valueSlot = cellSlot
		- ((intptr_t) valueRegister - (intptr_t) cellRegister);
	RawObject *cell = asObject(*cellSlot);
	rawObjectStoreValue(cell, &((RawCell *) cell)->value, *valueSlot);
	return *valueSlot;
}


// ---------------------------------------------------------------------------
// The compiler
// ---------------------------------------------------------------------------

typedef struct {
	MacroAssembler *assembler;
	CodeUnit *unit;
	MaLabel **labels;   // one per bytecode index, for branch targets
	uint32_t *machineOffsetAt;
	IcCell *cells;      // one per bytecode index, populated at send sites
} JitContext;


// NOT A SINGLE REGISTER NAME BELOW THIS LINE. Everything goes through the
// macro assembler, which is the only thing that knows what a register is
// (ADR 0009). The previous version of this file spelled RAX and RCX throughout
// and was, in effect, an x86-64 compiler wearing a general name.
static _Bool emitInstruction(JitContext *jit, size_t index, Opcode *unsupported)
{
	MacroAssembler *ma = jit->assembler;
	Instruction *instruction = &jit->unit->code[index];

	switch ((Opcode) instruction->op) {
	case OP_MOVE:
		maLoadSlot(ma, instruction->b);
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADI:
		// The 16-bit field is SIGNED, and sign-extending before tagging is what
		// makes a negative literal work: tagging first shifts the sign bit into
		// the payload.
		maLoadImmediate(ma, tagInt((int16_t) instruction->b));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADK:
		// Through the unit's literal field, for the same reason OP_GETGLOBAL is:
		// the Array moves, the C struct pointing at it does not, and that one
		// word is kept current by rootsVisitCompiledCode. Baking the element's
		// address would be a pointer the next collection invalidates.
		maLoadAbsolute(ma, &jit->unit->literals);
		maLoadField(ma, (uint16_t) (instruction->b + 1));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADNIL:
		maLoadImmediate(ma, tagPtr(Handles.nil.raw));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADTRUE:
		maLoadImmediate(ma, tagPtr(Handles.true_.raw));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_LOADFALSE:
		maLoadImmediate(ma, tagPtr(Handles.false_.raw));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_GETIVAR:
		maLoadSlot(ma, instruction->b);
		maLoadField(ma, instruction->c);
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_SETIVAR:
		// PENDING: the write barrier.
		maStoreField(ma, instruction->a, instruction->b, instruction->c);
		return 1;

	case OP_GETGLOBAL:
		// literals[b] is the global's Association, and its value is field 1.
		//
		// The literal frame is reached through the ADDRESS OF THE UNIT'S FIELD
		// rather than by baking the Array's address, and that is the whole point:
		// the Array moves whenever the collector feels like it, but the C struct
		// holding the reference does not, and rootsVisitCompiledCode keeps that
		// one word current. So the load is always of a live pointer, and no
		// baked address ever has to be relocated.
		maLoadAbsolute(ma, &jit->unit->literals);
		maLoadField(ma, (uint16_t) (instruction->b + 1)); // element b of the Array
		maLoadField(ma, 1);                               // the Association's value
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_SETGLOBAL:
		// Through the runtime, not inline, because the store needs the
		// generational write barrier and a global Association is old almost by
		// definition while what it is being pointed at is usually young. Globals
		// are also not a hot path: reading one is, writing one is not.
		maCallRuntime3(ma, jitStoreGlobal, jit->unit, instruction->b,
			instruction->a);
		return 1;

	case OP_JUMP:
		maJump(ma, jit->labels[instruction->a]);
		return 1;

	case OP_JUMPFALSE:
		maBranchIfImmediate(ma, instruction->a, tagPtr(Handles.false_.raw),
			MA_EQUAL, jit->labels[instruction->b]);
		return 1;

	case OP_JUMPTRUE:
		maBranchIfImmediate(ma, instruction->a, tagPtr(Handles.true_.raw),
			MA_EQUAL, jit->labels[instruction->b]);
		return 1;

	case OP_GUARDCLASS:
		maBranchIfNotClass(ma, instruction->a, (uint32_t) instruction->b,
			jit->labels[instruction->c]);
		return 1;

	case OP_SEND:
	case OP_SENDSUPER: {
		// Arguments live in CONSECUTIVE registers above the receiver, and slots
		// grow downward, so the ADDRESS of the receiver's slot is the whole
		// argument list. No marshalling, and nothing here knows how the target
		// passes arguments: that is the ABI's business.
		// argc in the low half, the receiver's REGISTER in the high half. The
		// register number is what lets jitDispatch turn the slot address it is
		// given back into a frame pointer, which is how a collection triggered
		// under a send finds the compiled frames beneath it. It rides in an
		// immediate that was already being materialised, so it costs nothing.
		//
		// A super send is the SAME sequence to a different runtime entry point,
		// so where the lookup starts costs nothing here and nothing at the site:
		// it was decided when this method was compiled and it lives in the cell.
		MaSendSite site = {
			.cell = &jit->cells[index],
			.miss = (Opcode) instruction->op == OP_SEND
				? jitDispatch : jitDispatchSuper,
			.receiverSlot = instruction->c,
			.argumentCount = instruction->n,
			.missInteger = (uint64_t) instruction->n
				| ((uint64_t) instruction->c << 32),
		};
		maSend(ma, &site);
		maStoreSlot(ma, instruction->a);
		return 1;
	}

	case OP_RET:
		maEpilogue(ma, instruction->a);
		return 1;

	case OP_RETOUTER:
		// Never comes back, so nothing follows it and no result is stored. The
		// register rides in the immediate for the same reason every other helper
		// takes it: it is what turns the slot address into the frame pointer,
		// and from there into the running closure in slot 0.
		maCallRuntime3(ma, jitReturnOuter, NULL, instruction->a,
			(uint64_t) instruction->a << 32);
		return 1;

	case OP_CLOSURE:
		// blocks[b], capturing the n registers starting at c. The capture list
		// was gathered into consecutive registers by the front end, for the same
		// reason a send's arguments are: one address describes the whole run.
		maCallRuntime3(ma, jitMakeClosure, jit->unit, instruction->c,
			(uint64_t) instruction->b | ((uint64_t) instruction->n << 16)
			| ((uint64_t) instruction->c << 32));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_GETUP:
		// Register 0 of a block's frame IS the closure, so a captured value is
		// one load at a known offset. This is what replaces the old VM's walk up
		// a chain of contexts (ADR 0008).
		maLoadSlot(ma, 0);
		maLoadField(ma, CLOSURE_CAPTURE_FIELD(instruction->b));
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_NEWCELL:
		maCallRuntime3(ma, jitMakeCell, NULL, instruction->b,
			(uint64_t) instruction->b << 32);
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_GETCELL:
		maLoadSlot(ma, instruction->b);
		maLoadField(ma, CELL_VALUE_FIELD);
		maStoreSlot(ma, instruction->a);
		return 1;

	case OP_SETCELL:
		// Through the runtime because the store needs the generational write
		// barrier, and a cell is exactly the object that outlives its frame.
		maCallRuntime3(ma, jitSetCell, NULL, instruction->a,
			(uint64_t) instruction->b | ((uint64_t) instruction->a << 32));
		return 1;

	case OP_SAFEPOINT:
		maSafepointPoll(ma, &CurrentThread.heap->safepointRequested);
		return 1;

	default:
		*unsupported = (Opcode) instruction->op;
		return 0;
	}
}


NativeCode *jitCompile(CodeUnit *unit, Opcode *unsupported)
{
	return jitCompileFor(maHostBackend(), unit, unsupported);
}


// Compile for a SPECIFIC backend. The host case is what runs; a foreign backend
// is what the cross-emission test uses to check byte-for-byte output on a
// machine that cannot execute it.
// The three singletons whose addresses generated code BAKES: nil in the
// prologue's slot fill and in OP_LOADNIL, true and false in OP_LOADTRUE /
// OP_LOADFALSE and in every inlined conditional's compare.
//
// Baking is legal ONLY because they never move (memory/Heap.h,
// allocateImmortalObject). This is asserted at EVERY compilation rather than
// trusted to a bootstrap, because the failure is silent: everything works until
// the first collection moves the singleton, after which a value that IS false
// stops matching the baked false and the method falls into its mustBeBoolean
// path. Measured, not imagined -- it is how this assertion came to exist.
static void assertSingletonsAreImmortal(void)
{
	ASSERT(Handles.nil.raw != NULL && isOldObject(Handles.nil.raw));
	ASSERT(Handles.true_.raw != NULL && isOldObject(Handles.true_.raw));
	ASSERT(Handles.false_.raw != NULL && isOldObject(Handles.false_.raw));
}


NativeCode *jitCompileFor(const MacroAssemblerOps *ops, CodeUnit *unit,
	Opcode *unsupported)
{
	assertSingletonsAreImmortal();
	// Idempotent: a unit built by the front end registered itself when it was
	// built, and one written by hand in a test registers here.
	jitRegisterUnit(unit);
	JitContext jit;
	jit.unit = unit;
	jit.assembler = maCreate(ops, unit->registerCount, unit->argumentCount);
	jit.labels = calloc(unit->instructionCount, sizeof(MaLabel *));
	jit.machineOffsetAt = calloc(unit->instructionCount, sizeof(uint32_t));
	jit.cells = calloc(unit->instructionCount, sizeof(IcCell));
	ASSERT(jit.labels != NULL && jit.machineOffsetAt != NULL && jit.cells != NULL);
	// A cache cell per send site, its selector resolved once here rather than
	// on every execution.
	//
	// And, at a SUPER site, where the lookup starts: the class above the one that
	// DEFINED this method, as an index, which is stable and needs no relocation
	// (ADR 0005). It is resolved here because it is a property of the site and of
	// the compile, and deriving it at runtime is impossible: the receiver may be
	// an instance of a subclass, and starting from its class would find this very
	// method again.
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		if (!opcodeIsSend((Opcode) unit->code[i].op)) {
			continue;
		}
		jit.cells[i].selector = asObject(
			((RawArray *) asObject(unit->literals))->vars[unit->code[i].b]);
		jit.cells[i].lookupStart = CLASS_INDEX_INVALID;
		if ((Opcode) unit->code[i].op != OP_SENDSUPER
				|| !valueTypeOf(unit->ownerClass, VALUE_POINTER)) {
			continue;
		}
		RawClass *super = rawClassSuperclass((RawClass *) asObject(unit->ownerClass));
		if (super != NULL) {
			jit.cells[i].lookupStart = super->classIndex;
		}
	}
	for (size_t i = 0; i < unit->instructionCount; i++) {
		jit.labels[i] = maNewLabel(jit.assembler);
	}

	maPrologue(jit.assembler, tagPtr(Handles.nil.raw));

	// The primitive goes here, between the prologue and the first bytecode: the
	// frame is already built, so the primitive reads the receiver and arguments
	// straight out of their slots, and a failure falls through with the frame
	// exactly right for the method body that handles the general case.
	//
	// It is deliberately NOT in jitDispatch. Putting it inside the method means
	// an inline-cached call that jumps directly to this entry point still gets
	// the primitive, which is the whole point of having one.
	//
	// Note also what this does to the bci map: machineOffsetAt[0] points PAST
	// the primitive attempt, which is correct, because arriving at bci 0 through
	// deoptimization or OSR means the primitive has already failed once.
	//
	// A DECLARED but not yet IMPLEMENTED primitive emits nothing at all, and the
	// method runs its Smalltalk fallback. That is what lets the whole kernel
	// compile while the VM implements the primitives a few at a time: 173 names
	// are declared in packages/ and only some have C behind them
	// (runtime/Primitives.def).
	PrimitiveFunction primitive = unit->primitive != PRIM_NONE
		? primitiveFunctionAt((PrimitiveNumber) unit->primitive) : NULL;
	if (primitive != NULL) {
		maCallPrimitive(jit.assembler, primitive, unit->argumentCount);
	}

	_Bool ok = 1;
	for (size_t i = 0; i < unit->instructionCount && ok; i++) {
		// Bind BEFORE emitting: a backward branch to this index must land on
		// the first byte of its code, and recording the offset here is also
		// what builds the bci-to-machine map.
		maBind(jit.assembler, jit.labels[i]);
		jit.machineOffsetAt[i] = (uint32_t) maOffset(jit.assembler);
		ok = emitInstruction(&jit, i, unsupported);
	}

	if (!ok) {
		maDestroy(jit.assembler);
		free(jit.labels);
		free(jit.machineOffsetAt);
		free(jit.cells);
		return NULL;
	}

	NativeCode *code = calloc(1, sizeof(NativeCode));
	ASSERT(code != NULL);
	code->unit = unit;
	code->frameSlots = unit->registerCount;
	// RECORDED, not re-derived. The prologue above asked the same question of
	// the same assembler to decide what to emit; asking it again anywhere else
	// is how the two ends of a call come to disagree.
	code->wide = maUsesWideArguments(jit.assembler);
	code->machineOffsetAt = jit.machineOffsetAt;
	code->cells = jit.cells;
	// THE FRAME DESCRIPTION, and for this tier it is one line because the tier
	// is uniform: every slot holds a tagged Value. Written out rather than
	// assumed, because "assumed" is what it was -- a sentence in a comment that
	// the collector had no way to read and no way to check.
	size_t mapBytes = sizeof(FrameMap) + frameMapByteCount(code->frameSlots);
	code->frameMap = calloc(1, mapBytes);
	if (code->frameMap == NULL) {
		FAIL();
	}
	code->frameMap->codeOffset = 0;
	code->frameMap->slotCount = code->frameSlots;
	code->frameMap->byteCount = (uint16_t) frameMapByteCount(code->frameSlots);
	for (uint16_t slot = 0; slot < code->frameSlots; slot++) {
		frameMapSetKind(code->frameMap, slot, SLOT_POINTER);
	}
	if (ops == maHostBackend()) {
		code->entry = maPublish(jit.assembler, &code->size);
	} else {
		// Foreign target: keep the bytes for inspection and never map them
		// executable.
		size_t size;
		const uint8_t *bytes = maBytes(jit.assembler, &size);
		code->size = size;
		code->entry = malloc(size);
		memcpy(code->entry, bytes, size);
	}

	maDestroy(jit.assembler);
	free(jit.labels);

	// On the root list LAST, once every field the visitor reads is populated: a
	// collection triggered between the calloc and here would walk a half-built
	// entry.
	compiledCodeRegister(code);
	tier2DryRun(unit);
	return code;
}


// ---------------------------------------------------------------------------
// Calling in
// ---------------------------------------------------------------------------

typedef Value (*Entry0)(Value);
typedef Value (*Entry1)(Value, Value);
typedef Value (*Entry2)(Value, Value, Value);
typedef Value (*Entry3)(Value, Value, Value, Value);
typedef Value (*Entry4)(Value, Value, Value, Value, Value);
typedef Value (*Entry5)(Value, Value, Value, Value, Value, Value);
// The WIDE entry, and there is only ONE of it however many arguments there are.
// That is the point of the convention: the arguments never become C arguments,
// so no per-arity variant and no ceiling (jit/Jit.h).
typedef Value (*EntryWide)(Value *);


// Entering compiled code, and the ONE place a non-local return can land.
//
// The record has to be made in THIS frame rather than inside a helper, because a
// jump resumes in the frame that took the destination: taking it one call deeper
// would resume in a frame that no longer exists. That is what the macro is for,
// and why it expands into each entry point instead of being a function of its
// own.
//
// The test is a field of the unit the compiler already filled, so a method with
// no `^` inside a block enters exactly as it did before.
#define ENTER_COMPILED(call) \
	if (!code->unit->couldBeHome) { \
		return (call); \
	} \
	UnwindRecord record; \
	unwindPush(&record); \
	if (setjmp(record.destination) != 0) { \
		return unwindAnswer(&record); \
	} \
	Value answer = (call); \
	unwindPop(&record); \
	return answer;

// Every positional entry asserts the code is NARROW. Handing a wide method its
// receiver in argument register 0 would have the prologue treat that Value as
// the address of an argument block and dereference it, which is a wild read
// from a tagged integer rather than a wrong answer -- but only sometimes, and
// only for the methods whose arity happens to cross the ABI's register set.
#define NARROW_ONLY() ASSERT(!code->wide)


Value jitCall0(NativeCode *code, Value receiver)
{
	NARROW_ONLY();
	Entry0 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver))
}


Value jitCall1(NativeCode *code, Value receiver, Value a)
{
	NARROW_ONLY();
	Entry1 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a))
}


Value jitCall2(NativeCode *code, Value receiver, Value a, Value b)
{
	NARROW_ONLY();
	Entry2 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a, b))
}


// Three, four and five arguments. The ceiling is the ABI's: the receiver plus
// five is the SysV integer argument set (JIT_MAX_REGISTER_ARGS), and a wider
// send needs its arguments in memory, which is a calling convention this tier
// does not have yet and says so about rather than guesses at.
Value jitCall3(NativeCode *code, Value receiver, Value a, Value b, Value c)
{
	NARROW_ONLY();
	Entry3 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a, b, c))
}


Value jitCall4(NativeCode *code, Value receiver, Value a, Value b, Value c, Value d)
{
	NARROW_ONLY();
	Entry4 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a, b, c, d))
}


Value jitCall5(NativeCode *code, Value receiver, Value a, Value b, Value c, Value d,
	Value e)
{
	NARROW_ONLY();
	Entry5 entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiver, a, b, c, d, e))
}


// The wide entry. `receiverSlot` points at the receiver, with the arguments at
// DESCENDING addresses from it; the prologue copies them straight into its own
// slots, so they must still be there and still be right at the moment of the
// call. For a send that is the caller's own frame and nothing has to be built;
// for a C caller it is a block laid out on purpose, and nothing may allocate
// between laying it out and calling, because the collector does not know about
// it.
Value jitCallWide(NativeCode *code, Value *receiverSlot)
{
	ASSERT(code->wide);
	EntryWide entry;
	memcpy(&entry, &code->entry, sizeof(entry));
	ENTER_COMPILED(entry(receiverSlot))
}


void jitFreeNativeCode(NativeCode *code)
{
	compiledCodeUnregister(code);
	// The machine code itself is NOT freed: exec memory is never reclaimed, so
	// a frame still running inside it stays valid forever.
	free(code->machineOffsetAt);
	free(code->frameMap);
	free(code->cells);
	free(code);
}


