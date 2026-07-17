#ifndef INLINE_CACHE_H
#define INLINE_CACHE_H

// Per-call-site inline caches for the dynamic send path (generateSend only:
// the DNU stub, outer-return and block-value primitives share one emission
// across every site, so a cache there would be megamorphic by construction).
//
// Concurrency contract (the hard invariant of this feature, see the torn-read
// history in core/Lookup.h): an IcState is IMMUTABLE once published, a cell is
// (re)pointed only by a single aligned word store (CAS on the bind path, plain
// store by the stop-the-world collectors), and readers reach class/target
// through the state pointer they loaded (address dependence: naturally ordered
// on POWER, program order on x86). A (class, target) pair is NEVER rewritten in
// place. Cells are DATA read by ordinary loads, so no icache flush is involved.
//
// ST_NO_IC=1  kill-switch: emit exactly the pre-IC send sequence, no cells.
// ST_IC_STATS=1  counters + exit dump; the JIT-emitted increments cost only
// under this flag. Counters are non-atomic (exact under a single worker).

#include "core/Object.h"
#include "runtime/String.h"
#include <stdlib.h>
#include <stdint.h>

struct NativeCode; // core/CompiledCode.h (which includes this header)

typedef struct IcState {
	Value class;      // TAGGED class pointer bound at this site (0 in gIcUnlinked)
	uint8_t *target;  // native code entry to call for that class
} IcState;

typedef struct IcCell {
	IcState *state;   // the ONLY word the JIT fast path reads; &gIcUnlinked or mono
} IcCell;

// Shared sentinel: class 0 never equals a tagged class pointer, so the guard
// always misses, and the emitted miss path tells "unlinked" from "bound to
// another class" by state->class == 0 without materializing this address.
extern IcState gIcUnlinked;

typedef struct {
	size_t sites;          // cells created (one per dynamic send site compiled)
	size_t hits;           // JIT-incremented, only emitted under ST_IC_STATS
	size_t polyFallbacks;  // JIT-incremented, only emitted under ST_IC_STATS
	size_t missCold;       // inlineCacheMiss calls (cell observed unlinked by JIT)
	size_t binds;          // successful CAS publishes
	size_t bindRaces;      // CAS lost to a peer, or peer already bound our class
	size_t polyPending;    // miss found another class bound (M2 promotes to PIC)
	size_t resetSweeps;    // STW invalidation sweeps over the exec space
	size_t cellsReset;     // bound cells returned to unlinked by those sweeps
	size_t stateBytesLive; // gauge: malloc'd IcState bytes currently published
} IcStats;
extern IcStats gIcStats;

uint8_t *inlineCacheMiss(RawClass *class, RawString *selector, IcCell *cell);
void icResetNativeCodeCells(struct NativeCode *code); // STW collectors only
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
