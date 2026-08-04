#ifndef COLLECTOR_H
#define COLLECTOR_H

// The two collections.
//
// `collectorScavenge` copies the live young objects, ages them once, and
// promotes what survives twice. `collectorMarkSweep` marks transitively from
// the roots and sweeps the non-moving old space into free lists.
//
// NOTHING IN HERE REPAIRS A SLOT. The old collector silently rewrote to nil any
// stack slot whose contents did not look like a live object, because its
// stackmaps came from a control-flow-blind linear scan and were routinely
// wrong. That made a missing map entry produce no test failure, which is
// exactly the bug class a dry cut cannot afford (ADR 0002, ADR 0003 R2). Here a
// slot that disagrees with its map is an assertion, and the map is exact by
// construction.

#include "memory/Heap.h"
#include "memory/Roots.h"

void collectorScavenge(Heap *heap);
void collectorMarkSweep(Heap *heap);

// Walk every root the VM has: the class table, the per-heap handles, and each
// mutator's handle scopes, thread-held Values and native frames. Exposed so the
// memory self-test can drive a collection with no execution engine present,
// which is gate level 1 (docs/jit-v2/01-gate.md).
void collectorVisitRoots(Heap *heap, RootVisitor visit, void *ctx);

// Extra root source for the self-test and for embedders: an array of Values the
// collector must treat as live. NULL disables it.
void collectorSetExtraRoots(Value *roots, size_t count);

#endif
