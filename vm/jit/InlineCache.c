#include "jit/InlineCache.h"
#include "core/CompiledCode.h"
#include "core/Lookup.h"
#include "core/Handle.h"
#include "core/Class.h"
#include "core/Thread.h"
#include "memory/Heap.h"
#include "memory/HeapPage.h"
#include <stdio.h>

IcState gIcUnlinked = { 0, NULL, IC_KIND_UNLINKED, 0, NULL };
IcState gIcMega = { 0, NULL, IC_KIND_MEGA, 0, NULL };
IcStats gIcStats = { 0 };

static size_t icStateSize(IcState *state);
static IcState *icAllocState(uintptr_t kind, uintptr_t size, IcState *prev);
static _Bool icPublish(IcCell *cell, IcState *expected, IcState *fresh);


// The shared miss/transition handler behind the PicProbeStub's C path: the
// stub already routed way hits and mega cells, so this runs only for an
// unlinked cell, a way-0 miss on a mono, an exhausted pic walk, or the race
// echoes of any of those. Every publish is a FRESH immutable state swung in
// by one CAS; the superseded state is chained on `prev` for the next STW
// sweep to free. Raw selector is handled before anything can GC, mirroring
// lookupNativeCode; the class arrives TAGGED straight from the guard.
uint8_t *inlineCacheMiss(Value taggedClass, RawString *selector, IcCell *cell)
{
	HandleScope scope;
	openHandleScope(&scope);
	Class *classHandle = scopeHandle((RawClass *) asObject(taggedClass));

	gIcStats.missCold++;
	// May allocate, compile and GC (parking this thread): the class may move
	// (handle keeps it current) and the cell may be reset or advanced by a
	// peer meanwhile. Probes the calling worker's TLS LookupCache first: the
	// global cache stays the fallback and the floor.
	NativeCodeEntry entry = cachedLookupNativeCode(classHandle->raw, selector);

	union PointerConverter converter;
	converter.function_pointer = entry;
	uint8_t *target = converter.object_pointer;
	Value current = tagPtr(classHandle->raw);

	// From here to the CAS there is no safepoint poll and no allocation, so no
	// collection can interleave: the class read off the handle cannot go stale
	// before it is published. Address dependence is not a C-level guarantee
	// (unlike in the emitted probe), hence the explicit acquire.
	IcState *seen = __atomic_load_n(&cell->state, __ATOMIC_ACQUIRE);
	if (seen == &gIcUnlinked || seen->kind == IC_KIND_UNLINKED) {
		// Either the shared sentinel or a malloc'd RETIRED state left by
		// icRetireCellsTargeting; the latter is chained for the STW sweep.
		IcState *fresh = icAllocState(IC_KIND_MONO, 1,
			seen == &gIcUnlinked ? NULL : seen);
		fresh->class = current;
		fresh->target = target;
		if (icPublish(cell, seen, fresh)) {
			gIcStats.binds++;
		}
	} else if (seen->kind == IC_KIND_MONO) {
		if (seen->class == current) {
			gIcStats.bindRaces++; // a peer bound our class between guard and here
		} else {
			// Promote to a 2-way pic. The OLD pair stays way 0 (the header the
			// inline guard reads): the first-bound class keeps the fast path.
			IcState *fresh = icAllocState(IC_KIND_PIC, 2, seen);
			fresh->ways[0].class = seen->class;
			fresh->ways[0].target = seen->target;
			fresh->ways[1].class = current;
			fresh->ways[1].target = target;
			fresh->class = fresh->ways[0].class;
			fresh->target = fresh->ways[0].target;
			if (icPublish(cell, seen, fresh)) {
				gIcStats.picBuilds++;
			}
		}
	} else if (seen->kind == IC_KIND_PIC) {
		_Bool covered = 0;
		for (uintptr_t i = 0; i < seen->size; i++) {
			covered |= seen->ways[i].class == current;
		}
		if (covered) {
			gIcStats.bindRaces++; // a peer's extension already has our class
		} else if (seen->size < IC_PIC_CAPACITY) {
			IcState *fresh = icAllocState(IC_KIND_PIC, seen->size + 1, seen);
			for (uintptr_t i = 0; i < seen->size; i++) {
				fresh->ways[i] = seen->ways[i];
			}
			fresh->ways[seen->size].class = current;
			fresh->ways[seen->size].target = target;
			fresh->class = fresh->ways[0].class;
			fresh->target = fresh->ways[0].target;
			if (icPublish(cell, seen, fresh)) {
				gIcStats.picExtends++;
			}
		} else {
			// Capacity reached: permanently megamorphic. The malloc'd mega
			// carries the superseded chain; the next STW sweep frees it and
			// parks the cell on the static gIcMega.
			IcState *fresh = icAllocState(IC_KIND_MEGA, 0, seen);
			if (icPublish(cell, seen, fresh)) {
				gIcStats.megaPromotes++;
			}
		}
	} else {
		gIcStats.bindRaces++; // a peer promoted to mega under us: nothing to do
	}

	closeHandleScope(&scope, NULL);
	return target;
}


static size_t icStateSize(IcState *state)
{
	return sizeof(IcState) + state->size * sizeof(IcWay);
}


static IcState *icAllocState(uintptr_t kind, uintptr_t size, IcState *prev)
{
	IcState *state = malloc(sizeof(IcState) + size * sizeof(IcWay));
	state->class = 0;
	state->target = NULL;
	state->kind = kind;
	state->size = size;
	state->prev = prev;
	return state;
}


// RELEASE publish: a reader that sees `fresh` sees its fields. The cell is
// repointed only here (CAS) and by the STW reset sweep. The loser frees its
// never-published state (no reader can hold it) and counts the race.
static _Bool icPublish(IcCell *cell, IcState *expected, IcState *fresh)
{
	if (__atomic_compare_exchange_n(&cell->state, &expected, fresh, 0,
			__ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
		gIcStats.stateBytesLive += icStateSize(fresh);
		return 1;
	}
	free(fresh);
	gIcStats.bindRaces++;
	return 0;
}


// Free a published state and every superseded predecessor chained on `prev`.
static void icFreeChain(IcState *state)
{
	while (state != NULL && state != &gIcUnlinked && state != &gIcMega) {
		IcState *prev = state->prev;
		gIcStats.stateBytesLive -= icStateSize(state);
		free(state);
		state = prev;
	}
}


// STW collectors only (scavenge, full GC, and the raw collectGarbage path):
// every mutator is parked or blocked, and neither the emitted guard, the
// PicProbeStub walk, nor the C handler holds an IcState pointer across a
// safepoint poll, so freeing here cannot race a reader. Resetting EVERY
// class-keyed cell (not only cells whose class moved or died) is required for
// correctness, not a shortcut: method ADDITION does not invalidate anything
// today (Class.c only rejects redefinition), so a cell surviving collections
// could stay stale forever; the full reset mirrors the epoch flush of the TLS
// LookupCache. Do NOT "optimize" this into a selective reset without adding
// install-time invalidation first. Mega cells hold no class pointers (their
// route is the class-independent global probe), so they stay mega: park them
// on the static sentinel and free the malloc'd carrier plus its chain.
void icResetNativeCodeCells(struct NativeCode *code)
{
	IcCell *cells = nativeCodeIcCells((NativeCode *) code);
	size_t count = ((NativeCode *) code)->icCellsSize;
	for (size_t i = 0; i < count; i++) {
		IcState *state = cells[i].state;
		if (state == &gIcUnlinked || state == &gIcMega) {
			continue;
		}
		if (state->kind == IC_KIND_MEGA) {
			icFreeChain(state);
			cells[i].state = &gIcMega;
			gIcStats.megaCells++;
		} else {
			icFreeChain(state);
			cells[i].state = &gIcUnlinked;
			gIcStats.cellsReset++;
		}
	}
}


// NON-STW retirement of every cell that dispatches into `oldCode`, called by
// the tier-1 recompiler (jit/Tier.c) right after it republishes a method, so
// callers rebind to the fresh code without waiting for the next scavenge.
// Runs under codegenLock, which serializes ALL NativeCode creation: the exec
// space cannot grow mid-walk, so the page iteration is stable without a
// stop-the-world. Retirement follows the cell invariants to the letter: the
// swing is a CAS to a FRESH immutable state (kind unlinked, class 0, so the
// inline guard misses and the PicProbeStub falls through to the C handler,
// which knows how to bind over it) that chains the superseded state on
// `prev`; only the STW sweep ever frees. A lost CAS means a peer rebound
// concurrently and is simply skipped (the next sweep evens it out). Peers
// with a stale TLS LookupCache may rebind a retired cell back to the old
// entry until their next epoch flush: documented staleness, not corruption.
// Install-time invalidation for method-dictionary mutation: class
// redefinition, additive extension, selector removal. Closes the staleness
// window icResetNativeCodeCells documents (method ADDITION invalidated
// nothing between scavenges). Stop the world exactly like objectBecome
// (core/Smalltalk.c): gcLock + safepoint park of every peer, run the caller's
// mutation on the frozen graph, then reset EVERY class-keyed IC cell in the
// exec space (satisfying icResetNativeCodeCells' STW-only precondition) and
// bump gcEpoch so parked peers flush their TLS LookupCaches on resume
// (lookupCacheOnGcResume); the calling worker flushes its own before
// resuming the world. Mega cells stay mega: their class-independent probe
// re-resolves through the flushed lookup path. The mutation callback MUST
// NOT allocate: the world is stopped with gcLock held.
void icInvalidateAllSends(void (*mutate)(void *ctx), void *ctx)
{
	Heap *heap = CurrentThread.heap;
	_Bool multi = (heap->mutators != NULL && heap->mutators->nextMutator != NULL);
	heapGcEnterBlocked(heap, &CurrentThread);
	pthread_mutex_lock(&heap->gcLock);
	heapGcLeaveBlocked(heap, &CurrentThread);
	if (multi) {
		heapGcBegin(heap, &CurrentThread);
	}
	heapFillAllTlabTails(heap);

	if (mutate != NULL) {
		mutate(ctx);
	}

	PageSpaceIterator iterator;
	pageSpaceIteratorInit(&iterator, &heap->execSpace);
	NativeCode *code = (NativeCode *) pageSpaceIteratorNext(&iterator);
	while (code != NULL) {
		if ((code->tags & TAG_FREESPACE) == 0) {
			icResetNativeCodeCells((struct NativeCode *) code);
		}
		code = (NativeCode *) pageSpaceIteratorNext(&iterator);
	}

	heap->gcEpoch++; // parked peers epoch-flush their TLS LookupCaches on resume
	CurrentThread.lookupCacheEpoch = heap->gcEpoch;
	flushLookupCache();

	if (multi) {
		heapGcEnd(heap);
	}
	pthread_mutex_unlock(&heap->gcLock);
}


void icRetireCellsTargeting(struct NativeCode *oldCode)
{
	uint8_t *entry = ((NativeCode *) oldCode)->insts;
	PageSpaceIterator iterator;
	pageSpaceIteratorInit(&iterator, &CurrentThread.heap->execSpace);
	NativeCode *code = (NativeCode *) pageSpaceIteratorNext(&iterator);

	gIcStats.retireWalks++;
	while (code != NULL) {
		if ((code->tags & TAG_FREESPACE) == 0) {
			IcCell *cells = nativeCodeIcCells(code);
			for (size_t i = 0; i < code->icCellsSize; i++) {
				IcState *seen = __atomic_load_n(&cells[i].state, __ATOMIC_ACQUIRE);
				if (seen == &gIcUnlinked || seen == &gIcMega
						|| seen->kind == IC_KIND_UNLINKED || seen->kind == IC_KIND_MEGA) {
					continue;
				}
				_Bool targets = 0;
				for (uintptr_t w = 0; w < seen->size; w++) {
					targets |= seen->kind == IC_KIND_MONO
						? seen->target == entry
						: seen->ways[w].target == entry;
				}
				if (targets && icPublish(&cells[i],
						seen, icAllocState(IC_KIND_UNLINKED, 0, seen))) {
					gIcStats.cellsRetired++;
				}
			}
		}
		code = (NativeCode *) pageSpaceIteratorNext(&iterator);
	}
}


void icPrintStats(void)
{
	printf("[IC] sites          %zu\n", gIcStats.sites);
	printf("[IC] hits           %zu\n", gIcStats.hits);
	printf("[IC] picHits        %zu\n", gIcStats.picHits);
	printf("[IC] megaProbes     %zu\n", gIcStats.megaProbes);
	printf("[IC] missCold       %zu\n", gIcStats.missCold);
	printf("[IC] binds          %zu\n", gIcStats.binds);
	printf("[IC] picBuilds      %zu\n", gIcStats.picBuilds);
	printf("[IC] picExtends     %zu\n", gIcStats.picExtends);
	printf("[IC] megaPromotes   %zu\n", gIcStats.megaPromotes);
	printf("[IC] bindRaces      %zu\n", gIcStats.bindRaces);
	printf("[IC] resetSweeps    %zu\n", gIcStats.resetSweeps);
	printf("[IC] cellsReset     %zu\n", gIcStats.cellsReset);
	printf("[IC] megaCells      %zu\n", gIcStats.megaCells);
	printf("[IC] stateBytesLive %zu\n", gIcStats.stateBytesLive);
	printf("[IC] retireWalks    %zu\n", gIcStats.retireWalks);
	printf("[IC] cellsRetired   %zu\n", gIcStats.cellsRetired);
}
