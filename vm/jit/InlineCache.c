#include "jit/InlineCache.h"
#include "core/CompiledCode.h"
#include "core/Lookup.h"
#include "core/Handle.h"
#include "core/Class.h"
#include <stdio.h>

IcState gIcUnlinked = { 0, NULL };
IcStats gIcStats = { 0 };


// The shared miss handler behind IcMissStub. Reached only when the JIT guard
// observed the cell UNLINKED (a bound-to-another-class miss falls through to
// the global-cache probe instead, the pre-IC sequence and performance floor).
// Raw class/selector are handled before anything can GC, mirroring
// lookupNativeCode.
uint8_t *inlineCacheMiss(RawClass *class, RawString *selector, IcCell *cell)
{
	HandleScope scope;
	openHandleScope(&scope);
	Class *classHandle = scopeHandle(class);

	gIcStats.missCold++;
	// May allocate, compile and GC (parking this thread): the class may move
	// (handle keeps it current) and the cell may be reset or bound by a peer
	// meanwhile. Probes the calling worker's TLS LookupCache first: the global
	// cache stays the fallback and the floor.
	NativeCodeEntry entry = cachedLookupNativeCode(class, selector);

	union PointerConverter converter;
	converter.function_pointer = entry;
	uint8_t *target = converter.object_pointer;
	Value taggedClass = tagPtr(classHandle->raw);

	// From here to the CAS there is no safepoint poll and no allocation, so no
	// collection can interleave: the class read off the handle cannot go stale
	// before it is published. Address dependence is not a C-level guarantee
	// (unlike in the emitted probe), hence the explicit acquire.
	IcState *seen = __atomic_load_n(&cell->state, __ATOMIC_ACQUIRE);
	if (seen == &gIcUnlinked) {
		IcState *fresh = malloc(sizeof(IcState));
		fresh->class = taggedClass;
		fresh->target = target;
		IcState *expected = &gIcUnlinked;
		// RELEASE publish: a reader that sees `fresh` sees its fields. The cell
		// is repointed only here (CAS) and by the STW reset sweep.
		if (__atomic_compare_exchange_n(&cell->state, &expected, fresh, 0,
				__ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
			gIcStats.binds++;
			gIcStats.stateBytesLive += sizeof(IcState);
		} else {
			free(fresh); // never published: no reader can hold it
			gIcStats.bindRaces++;
		}
	} else if (seen->class == taggedClass) {
		gIcStats.bindRaces++; // a peer bound our class between guard and here
	} else {
		gIcStats.polyPending++; // second class at this site: M2 promotes to PIC
	}

	closeHandleScope(&scope, NULL);
	return target;
}


// STW collectors only (scavenge, full GC, and the raw collectGarbage path):
// every mutator is parked or blocked, and neither the emitted guard nor the
// miss handler holds an IcState pointer across a safepoint poll, so freeing
// the bound state here cannot race a reader. Resetting EVERY bound cell (not
// only cells whose class moved or died) is required for correctness, not a
// shortcut: method ADDITION does not invalidate anything today (Class.c only
// rejects redefinition), so a cell surviving collections could stay stale
// forever; the full reset mirrors the epoch flush of the TLS LookupCache. Do
// NOT "optimize" this into a selective reset without adding install-time
// invalidation first.
void icResetNativeCodeCells(struct NativeCode *code)
{
	IcCell *cells = nativeCodeIcCells((NativeCode *) code);
	size_t count = ((NativeCode *) code)->icCellsSize;
	for (size_t i = 0; i < count; i++) {
		IcState *state = cells[i].state;
		if (state != &gIcUnlinked) {
			free(state);
			cells[i].state = &gIcUnlinked;
			gIcStats.cellsReset++;
			gIcStats.stateBytesLive -= sizeof(IcState);
		}
	}
}


void icPrintStats(void)
{
	printf("[IC] sites          %zu\n", gIcStats.sites);
	printf("[IC] hits           %zu\n", gIcStats.hits);
	printf("[IC] polyFallbacks  %zu\n", gIcStats.polyFallbacks);
	printf("[IC] missCold       %zu\n", gIcStats.missCold);
	printf("[IC] binds          %zu\n", gIcStats.binds);
	printf("[IC] bindRaces      %zu\n", gIcStats.bindRaces);
	printf("[IC] polyPending    %zu\n", gIcStats.polyPending);
	printf("[IC] resetSweeps    %zu\n", gIcStats.resetSweeps);
	printf("[IC] cellsReset     %zu\n", gIcStats.cellsReset);
	printf("[IC] stateBytesLive %zu\n", gIcStats.stateBytesLive);
}
