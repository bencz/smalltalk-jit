#include "memory/Collector.h"
#include "memory/ObjectWalk.h"
#include "core/Assert.h"
#include "core/Handle.h"
#include "core/Thread.h"
#include <stdlib.h>
#include "os/Os.h"
#include <string.h>

// Extra roots, for the self-test and for embedders holding Values in C.
static Value *gExtraRoots;
static size_t gExtraRootCount;

void collectorSetExtraRoots(Value *roots, size_t count)
{
	gExtraRoots = roots;
	gExtraRootCount = count;
}


// ---------------------------------------------------------------------------
// Root enumeration
// ---------------------------------------------------------------------------

static void visitClassTable(Heap *heap, RootVisitor visit, void *ctx)
{
	// The class table is a root set AND a relocation table: an entry is the only
	// reference some classes have, and every entry must be updated when its
	// class moves, or a 22-bit class index would name a corpse.
	ClassTable *classes = &heap->classes;
	for (size_t i = CLASS_INDEX_FIRST; i < classes->size; i++) {
		RawObject *entry = classes->entries[i];
		if (!classTableIsLive(entry)) {
			continue; // NULL, or an odd free-list link (never a heap pointer)
		}
		Value slot = tagPtr(entry);
		visit(ctx, &slot);
		classes->entries[i] = asObject(slot);
	}
}


void collectorVisitRoots(Heap *heap, RootVisitor visit, void *ctx)
{
	visitClassTable(heap, visit, ctx);
	if (heap->handles != NULL) {
		smalltalkHandlesVisitRoots(heap, visit, ctx);
	}
	// Literal frames and inline-cache selectors: heap references that compiled
	// code holds outside the heap, so nothing else reaches them (memory/Roots.h).
	rootsVisitCompiledCode(visit, ctx);
	// Every fiber that is not running: its state moved into the Fiber when it
	// switched off, so nothing else can reach it (memory/Roots.h).
	rootsVisitFibers(visit, ctx);

	for (size_t i = 0; i < gExtraRootCount; i++) {
		if (valueTypeOf(gExtraRoots[i], VALUE_POINTER)) {
			visit(ctx, &gExtraRoots[i]);
		}
	}

	// Per-mutator roots: the handle scopes C code is holding objects in, then
	// the native frames. Handles come FIRST because they are the ones that
	// exist during a collection triggered BY an allocation, which is the common
	// case: the allocating C function is holding its operands in a scope.
	for (struct Thread *t = heap->mutators; t != NULL; t = t->nextMutator) {
		handlesVisitRoots(t, visit, ctx);
		rootsVisitNativeFrames(t, visit, ctx);
		// The unwind chain: an on:do:'s class and handler block, an ensure:'s
		// cleanup block. They sit on C frames and nothing else can reach them.
		rootsVisitUnwindRecords(t, visit, ctx);
	}
}


// ---------------------------------------------------------------------------
// Young collection: Cheney copy with one aging step
// ---------------------------------------------------------------------------

typedef struct {
	Heap *heap;
	Nursery *nursery;
	// Objects PROMOTED into the old space by this collection.
	//
	// They are survivors exactly like the ones copied into to-space, and their
	// slots still point into the space being evacuated, but they do NOT sit in
	// the address range the Cheney loop walks, so address order cannot reach
	// them. They need an explicit work list, and leaving them out is a bug that
	// stays invisible for a long time: the promoted object is intact and the
	// collection reports success, and only a later read of one of its slots
	// finds a pointer into the dead semispace.
	RawObject **promoted;
	size_t promotedCount;
	size_t promotedCapacity;
} ScavengeContext;


static void promotedPush(ScavengeContext *sc, RawObject *object)
{
	if (sc->promotedCount == sc->promotedCapacity) {
		sc->promotedCapacity = sc->promotedCapacity == 0
			? 256 : sc->promotedCapacity * 2;
		sc->promoted = realloc(sc->promoted,
			sc->promotedCapacity * sizeof(RawObject *));
		ASSERT(sc->promoted != NULL);
	}
	sc->promoted[sc->promotedCount++] = object;
}


// Copy one from-space object out, or return where it already went. Objects that
// have survived a previous collection (GC_AGED) are promoted to the old space
// instead of copied again: two survivals is the whole aging policy.
static RawObject *evacuate(ScavengeContext *sc, RawObject *object)
{
	if (rawObjectIsForwarded(object)) {
		return rawObjectForwardTarget(object);
	}
	size_t size = objectSizeInBytes(&sc->heap->classes, object);
	// Objects too large to encode their size in the header never enter the
	// nursery at all (see allocate()), so no copy path here has to carry the
	// overflow size word that would otherwise sit in front of the header.
	ASSERT(!rawObjectIsBig(object));

	RawObject *copy;
	if (rawObjectHasGcBit(object, GC_AGED)) {
		copy = (RawObject *) pageSpaceAllocate(&sc->heap->oldSpace, size);
		ASSERT(copy != NULL);
		memcpy(copy, object, size);
		rawObjectClearGcBit(copy, GC_AGED);
		// Cleared, then re-established by the drain loop if it turns out to be
		// warranted: the copy inherits the source's bits, and a stale
		// GC_REMEMBERED would make rememberedSetAdd's assertion fire on an
		// object that is not in any set.
		rawObjectClearGcBit(copy, GC_REMEMBERED);
		// On the work list, because address order cannot find it: it went to the
		// old space, not into the to-space range the Cheney loop walks.
		if (formatHasPointers(rawObjectFormat(copy))) {
			promotedPush(sc, copy);
		}
		sc->nursery->promotedBytes += size;
	} else {
		copy = (RawObject *) sc->nursery->copyTop;
		sc->nursery->copyTop += size;
		ASSERT(sc->nursery->copyTop
			<= sc->nursery->toSpace + sc->nursery->semiSize);
		memcpy(copy, object, size);
		rawObjectSetGcBit(copy, GC_AGED);
		sc->nursery->survivorBytes += size;
	}
	rawObjectSetForward(object, copy);
	return copy;
}


static void scavengeSlot(void *ctx, Value *slot)
{
	ScavengeContext *sc = ctx;
	// The map said this slot is a pointer. It IS a pointer: anything else is a
	// broken map, and a broken map is a bug to find, not damage to paper over.
	ASSERT(valueTypeOf(*slot, VALUE_POINTER));
	RawObject *object = asObject(*slot);
	if (!nurseryInFromSpace(sc->nursery, (uint8_t *) object)) {
		return; // old, or already evacuated into to-space
	}
	*slot = tagPtr(evacuate(sc, object));
}


// Is this thread in the heap's mutator list?
//
// Asked so the heap's own thread's log is drained EXACTLY ONCE: it is normally a
// mutator like any other, but during an image load and in the self-tests that
// drive the collector directly it is not in the list at all. Draining a set twice
// is not merely wasteful -- scavengeRememberedSet DETACHES the chain and re-adds
// what is still young, so the second pass would walk objects whose slots have
// already been forwarded and re-add them against a set that no longer describes
// them.
static _Bool threadIsMutatorOf(Heap *heap, struct Thread *thread)
{
	for (struct Thread *t = heap->mutators; t != NULL; t = t->nextMutator) {
		if (t == thread) {
			return 1;
		}
	}
	return 0;
}


// Drain the old-to-young log: every old object that a write barrier recorded is
// scanned, and drops out of the set when it turns out to hold nothing young any
// more.
static void scavengeRememberedSet(ScavengeContext *sc, RememberedSet *set)
{
	// DETACH the chain before walking it. An old object that still points
	// somewhere young after the walk is re-added, and re-adding into the chain
	// being iterated either loses entries or visits them twice, depending on
	// whether the append happened to land in a block already passed. Detaching
	// makes the two sets disjoint by construction.
	RememberedSetBlock *chain = set->blocks;
	set->blocks = NULL;
	rememberedSetGrow(set);

	RememberedSetBlock *block = chain;
	while (block != NULL) {
		for (RawObject **p = block->objects; p < block->current; p++) {
			RawObject *object = *p;
			rawObjectClearGcBit(object, GC_REMEMBERED);
			size_t count;
			Value *slots = objectPointerSlots(&sc->heap->classes, object, &count);
			_Bool stillYoung = 0;
			for (size_t i = 0; i < count; i++) {
				if (!valueTypeOf(slots[i], VALUE_POINTER)) {
					continue;
				}
				scavengeSlot(sc, &slots[i]);
				stillYoung = stillYoung || isNewObject(asObject(slots[i]));
			}
			if (stillYoung) {
				rememberedSetAdd(set, object);
			}
		}
		block = block->prev;
	}
	rememberedSetFreeBlocks(chain);
}


void collectorScavenge(Heap *heap)
{
	int64_t started = osCurrentMicroTime();
	Nursery *nursery = &heap->newSpace;

	// FIRST, before anything else: every mutator's TLAB is a pointer into the
	// space we are about to evacuate, and the semispaces are about to swap
	// roles. A TLAB that survived this would keep handing out addresses inside
	// the NEXT collection's destination, and that collection would copy
	// survivors over live objects. Owned here rather than by the caller,
	// because a caller that forgets produces silent aliasing rather than a
	// crash.
	heapFillAllTlabTails(heap);

	// `fromSpace` is where the mutators were allocating, so it is the space to
	// EVACUATE, and it keeps that name for the whole collection: every "does
	// this pointer still need forwarding" test reads it. Survivors go to
	// `toSpace`, and only at the very end do the two swap roles.
	uint8_t *destination = nursery->toSpace;
	uintptr_t base = ((uintptr_t) destination + (HEAP_OBJECT_ALIGN - 1))
		& ~(uintptr_t) (HEAP_OBJECT_ALIGN - 1);
	nursery->copyTop = (uint8_t *) (base | NEW_SPACE_TAG);
	nursery->scan = nursery->copyTop;
	nursery->survivorBytes = 0;
	nursery->promotedBytes = 0;

	ScavengeContext sc = { .heap = heap, .nursery = nursery };
	collectorVisitRoots(heap, scavengeSlot, &sc);
	// EVERY MUTATOR'S LOG, not just the heap thread's.
	//
	// The barrier is per-mutator (core/Thread.h) because making it shared would
	// put a lock on every pointer store in the system. The consequence is that the
	// old-to-young edges a WORKER recorded live in that worker's set, and reading
	// only heap->thread's means a collection cannot see them.
	//
	// What that costs is not a slow path, it is a reclaimed live object: an old
	// object holding the only reference to a young one is exactly what the barrier
	// exists to record, so an unread log means the young object has no root, is
	// evacuated as garbage, and the old object keeps pointing where it used to be.
	//
	// Measured: the multi-worker Atomics stress builds a Treiber stack whose cell
	// is old and whose nodes are young, pushed from several workers at once. It
	// reported lost and duplicated nodes -- a wrong sum and a wrong count, never a
	// crash, which is why nothing else in the suite could see it.
	//
	// Safe to walk the mutator list here: the world is stopped for the duration of
	// a collection, so no mutator is joining, leaving or logging.
	for (struct Thread *t = heap->mutators; t != NULL; t = t->nextMutator) {
		scavengeRememberedSet(&sc, &t->rememberedSet);
	}
	// And the heap's own thread when it is not a mutator in that list, which is
	// the case during a load and in the self-tests that drive the collector
	// without ever registering.
	if (heap->thread != NULL && !threadIsMutatorOf(heap, heap->thread)) {
		scavengeRememberedSet(&sc, &heap->thread->rememberedSet);
	}

	// TWO work lists, drained together until BOTH are empty.
	//
	// Cheney's trick is that everything copied into to-space is itself a source
	// of roots and lies in address order, so the scan pointer is the work list.
	// That holds only for objects that went to TO-SPACE. An object promoted to
	// the old space is just as much a survivor with just as many stale slots,
	// and it is nowhere in that range, so it comes off the explicit list built
	// during evacuation.
	//
	// They alternate rather than run in sequence because each feeds the other:
	// scanning a promoted object can evacuate more objects, and evacuating can
	// promote more.
	for (;;) {
		if (nursery->scan < nursery->copyTop) {
			RawObject *object = (RawObject *) nursery->scan;
			size_t count;
			Value *slots = objectPointerSlots(&heap->classes, object, &count);
			for (size_t i = 0; i < count; i++) {
				if (valueTypeOf(slots[i], VALUE_POINTER)) {
					scavengeSlot(&sc, &slots[i]);
				}
			}
			nursery->scan += objectSizeInBytes(&heap->classes, object);
			continue;
		}
		if (sc.promotedCount > 0) {
			RawObject *object = sc.promoted[--sc.promotedCount];
			size_t count;
			Value *slots = objectPointerSlots(&heap->classes, object, &count);
			_Bool pointsYoung = 0;
			for (size_t i = 0; i < count; i++) {
				if (!valueTypeOf(slots[i], VALUE_POINTER)) {
					continue;
				}
				scavengeSlot(&sc, &slots[i]);
				pointsYoung = pointsYoung || isNewObject(asObject(slots[i]));
			}
			// It is OLD now, so any surviving edge into the young space is an
			// old-to-young edge that no write barrier ran for. Logging it here
			// is what keeps that edge visible to the NEXT collection, which will
			// no longer have this object in its copied set.
			if (pointsYoung && heap->thread != NULL
					&& !rawObjectHasGcBit(object, GC_REMEMBERED)) {
				rememberedSetAdd(&heap->thread->rememberedSet, object);
			}
			continue;
		}
		break;
	}
	free(sc.promoted);

	// Now the roles swap: the space we just filled with survivors is where the
	// mutators allocate next, and the emptied one becomes the next destination.
	nursery->toSpace = nursery->fromSpace;
	nursery->fromSpace = destination;
	nursery->top = nursery->copyTop;
	nursery->limit = nursery->fromSpace + nursery->semiSize;
	nursery->scan = NULL;
	nursery->copyTop = NULL;

	heap->gcEpoch++;
	LastGCStats.scavengeCount++;
	LastGCStats.scavengeTimeUs += osCurrentMicroTime() - started;
	LastGCStats.youngSurvivorBytes = (int64_t) nursery->survivorBytes;
}


// ---------------------------------------------------------------------------
// Full collection: mark transitively, sweep the non-moving old space
// ---------------------------------------------------------------------------

typedef struct {
	Heap *heap;
	RawObject **stack;
	size_t count;
	size_t capacity;
} MarkContext;


static void markPush(MarkContext *mc, RawObject *object)
{
	if (mc->count == mc->capacity) {
		mc->capacity = mc->capacity == 0 ? 1024 : mc->capacity * 2;
		mc->stack = realloc(mc->stack, mc->capacity * sizeof(RawObject *));
		ASSERT(mc->stack != NULL);
	}
	mc->stack[mc->count++] = object;
}


static void markSlot(void *ctx, Value *slot)
{
	MarkContext *mc = ctx;
	ASSERT(valueTypeOf(*slot, VALUE_POINTER));
	RawObject *object = asObject(*slot);
	if (rawObjectHasGcBit(object, GC_MARKED)) {
		return;
	}
	rawObjectSetGcBit(object, GC_MARKED);
	if (formatHasPointers(rawObjectFormat(object))) {
		markPush(mc, object);
	}
}


void collectorMarkSweep(Heap *heap)
{
	int64_t started = osCurrentMicroTime();

	// A full collection evacuates the young space first, twice, so that
	// afterwards every live object is in the OLD space and a single mark bit
	// answers for the whole heap. Two passes because the aging policy promotes
	// on the second survival: the first pass ages, the second promotes. Blunt,
	// and it costs one extra copy of the live young set per full collection;
	// worth revisiting only if a full collection shows up in a profile, which
	// on this workload it should not (they are rare by construction).
	collectorScavenge(heap);
	collectorScavenge(heap);

	MarkContext mc = { .heap = heap, .stack = NULL, .count = 0, .capacity = 0 };
	collectorVisitRoots(heap, markSlot, &mc);

	while (mc.count > 0) {
		RawObject *object = mc.stack[--mc.count];
		size_t count;
		Value *slots = objectPointerSlots(&heap->classes, object, &count);
		for (size_t i = 0; i < count; i++) {
			if (valueTypeOf(slots[i], VALUE_POINTER)) {
				markSlot(&mc, &slots[i]);
			}
		}
	}
	free(mc.stack);

	size_t reclaimed = pageSpaceSweep(&heap->oldSpace, &heap->classes);

	LastGCStats.count++;
	LastGCStats.freed += reclaimed;
	LastGCStats.time = osCurrentMicroTime() - started;
	LastGCStats.totalTime += LastGCStats.time;
}
