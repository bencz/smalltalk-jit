#ifndef INLINE_CACHE_H
#define INLINE_CACHE_H

// Per-call-site inline caches for the dynamic send path (generateSend only:
// the DNU stub, outer-return and block-value primitives share one emission
// across every site, so a cache there would be megamorphic by construction).
//
// Concurrency contract (the hard invariant of this feature, see the torn-read
// history in core/Lookup.h): an IcState is IMMUTABLE once published, a cell is
// (re)pointed only by a single aligned word store (CAS on the bind/transition
// path, plain store by the stop-the-world collectors), and readers reach every
// field through the state pointer they loaded (address dependence: naturally
// ordered on POWER, program order on x86). A (class, target) pair is NEVER
// rewritten in place. Cells are DATA read by ordinary loads: no icache flush.
//
// State lattice per cell, all transitions CAS-published fresh allocations:
//   gIcUnlinked -> mono -> pic(2..IC_PIC_CAPACITY ways) -> mega (permanent).
// The superseded state is chained on `prev` (set at CREATION, never mutated)
// and the whole chain is freed by the STW reset sweep, so transitions leak
// nothing. The reset returns mono/pic cells to gIcUnlinked (class addresses
// die with the epoch) but keeps mega cells mega (their target is the
// class-independent global probe): mega is permanent by design.
//
// ST_NO_IC=1  kill-switch: emit exactly the pre-IC send sequence, no cells.
// ST_IC_STATS=1  counters + exit dump; the JIT-emitted increments cost only
// under this flag. Counters are non-atomic (exact under a single worker).

#include "core/Object.h"
#include "runtime/String.h"
#include <stdlib.h>
#include <stdint.h>

struct NativeCode; // core/CompiledCode.h (which includes this header)

#define IC_PIC_CAPACITY 6

enum {
	IC_KIND_UNLINKED,
	IC_KIND_MONO,
	IC_KIND_PIC,
	IC_KIND_MEGA,
};

typedef struct {
	Value class;      // TAGGED class of this way
	uint8_t *target;  // native code entry for that class
} IcWay;

typedef struct IcState {
	// Way 0 duplicated in the header at offsets 0/8: the JIT's inline guard
	// reads ONLY these two words, so a mono state and a pic's hottest way
	// dispatch identically, and the sentinels (class 0) always miss it.
	Value class;
	uint8_t *target;
	uintptr_t kind;         // IC_KIND_*
	uintptr_t size;         // ways count: 1 mono, 2..IC_PIC_CAPACITY pic, else 0
	struct IcState *prev;   // superseded predecessor chain, freed at STW reset
	IcWay ways[];           // pic only; ways[0] mirrors the header pair
} IcState;

typedef struct IcCell {
	IcState *state;   // the ONLY word the JIT fast path reads
} IcCell;

// Shared sentinels: class 0 never equals a tagged class pointer, so the
// inline guard always misses into the PicProbeStub, which discriminates on
// `kind`. gIcMega is what the STW reset leaves in a promoted cell.
extern IcState gIcUnlinked;
extern IcState gIcMega;

typedef struct {
	size_t sites;          // cells created (one per dynamic send site compiled)
	size_t hits;           // way-0 hits, JIT-emitted only under ST_IC_STATS
	size_t picHits;        // way 1..n-1 hits in the stub walk, same gating
	size_t megaProbes;     // mega cells routed to the global probe, same gating
	size_t missCold;       // C handler calls (bind, transition, or race echo)
	size_t binds;          // unlinked -> mono publishes
	size_t picBuilds;      // mono -> 2-way pic publishes
	size_t picExtends;     // pic n -> n+1 publishes
	size_t megaPromotes;   // pic at capacity -> mega publishes
	size_t bindRaces;      // CAS lost, or a peer already covered our class
	size_t resetSweeps;    // STW invalidation sweeps over the exec space
	size_t cellsReset;     // mono/pic cells returned to unlinked by the sweeps
	size_t megaCells;      // cells currently parked on the mega sentinel
	size_t stateBytesLive; // gauge: malloc'd IcState bytes currently reachable
	size_t retireWalks;    // exec-space walks by icRetireCellsTargeting
	size_t cellsRetired;   // cells CAS-swung to a retired unlinked state
} IcStats;
extern IcStats gIcStats;

uint8_t *inlineCacheMiss(Value taggedClass, RawString *selector, IcCell *cell);
void icResetNativeCodeCells(struct NativeCode *code); // STW collectors only
void icRetireCellsTargeting(struct NativeCode *oldCode); // codegenLock holders only
// STW bracket for method-dict mutation: parks peers, runs `mutate`, resets
// every IC cell + flushes lookup caches. Callback must not allocate.
void icInvalidateAllSends(void (*mutate)(void *ctx), void *ctx);
void icPrintStats(void);


static _Bool icEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_NO_IC") == NULL;
	}
	return enabled;
}


static _Bool icStatsEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_IC_STATS") != NULL;
	}
	return enabled;
}

#endif
