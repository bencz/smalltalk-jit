#ifndef HEAP_H
#define HEAP_H

// One heap per isolate, shared by that isolate's worker OS threads.
//
// Allocation is a per-mutator bump inside a TLAB carved from the young space;
// only the refill takes a lock. Collection is stop-the-world across the heap's
// mutators: young collections copy and age, full collections mark and sweep the
// non-moving old space.
//
// This header deliberately does NOT include core/Thread.h: Thread embeds a TLAB
// and a RememberedSet and therefore includes this file. The dependency runs one
// way, and `struct Thread` stays opaque here.

#include "core/ClassTable.h"
#include "core/Tls.h"
#include "core/Object.h"
#include "memory/Nursery.h"
#include "memory/PageSpace.h"
#include "memory/RememberedSet.h"
#include "os/OsThread.h"

struct Thread;
struct Scheduler;
struct SmalltalkHandles;

typedef struct {
	size_t count;              // full collections
	size_t marked;
	size_t sweeped;
	size_t freed;
	size_t extended;
	size_t total;
	int64_t time;
	int64_t totalTime;
	// Young collections. Cumulative and NOT cleared by resetGcStats, so deltas
	// across a stress wave reveal whether per-collection cost is climbing.
	size_t scavengeCount;
	int64_t scavengeTimeUs;
	int64_t youngSurvivorBytes;
} GCStats;

extern PER_ISOLATE GCStats LastGCStats;

typedef struct Heap {
	struct Thread *thread;
	// Per-heap well-known handles (kernel classes, the Smalltalk global
	// dictionary, the symbol table), shared by every worker of this heap.
	struct SmalltalkHandles *handles;
	// Class index -> class object. A ROOT SET, and the reason a class can be
	// named by a 22-bit immediate in generated code (ADR 0005).
	ClassTable classes;

	Nursery newSpace;
	PageSpace oldSpace;
	PageSpace execSpace;

	size_t oldGcThreshold; // full GC only once old space grows past this
	// Bumped once per young collection under the stop-the-world. Mutators
	// compare it against their own epoch on resume to flush caches that a
	// collection may have invalidated.
	size_t gcEpoch;

	// Guards carving TLABs out of the shared young space. The per-mutator bump
	// INSIDE a TLAB stays lock-free; only the (rare) refill takes this.
	OsMutex youngLock;
	OsMutex oldLock;      // old space free lists and page growth
	OsMutex execLock;     // executable space, shared by lazily compiling workers
	OsMutex codegenLock;  // serializes code generation across workers
	OsMutex symbolLock;   // the shared symbol table
	OsMutex gcLock;
	OsMutex safepointLock;
	OsCond safepointCond;
	volatile int safepointRequested;

	// Striped synchronization monitor for the Smalltalk sync primitives. The
	// stripe for an object is derived from its identity hash and stashed in the
	// Thread, never recomputed, so enter and exit provably drop the same lock
	// even if the object moved or was become:-d mid critical section.
	OsMutex *monitorLocks;
	size_t monitorStripeCount;
	size_t monitorStripeShift;

	size_t symbolCount;
	_Bool symbolCountValid;

	// Every OS thread mutating THIS heap links itself here so a collection can
	// scan the roots of all of them, not just the collecting thread.
	struct Thread *mutators;
	struct Scheduler *sched;
} Heap;

void initHeap(Heap *heap, struct Thread *thread);
void freeHeap(Heap *heap);

// Raw allocation of `bytes` (caller-aligned). Collects when the young space is
// exhausted, and falls back to old space for objects too large to be worth a
// nursery.
uint8_t *allocate(Heap *heap, size_t bytes);
// Allocate and stamp the header for an instance of `class` with `elements`
// indexed elements.
RawObject *allocateObject(Heap *heap, RawObject *class, size_t elements);

void collectGarbage(Heap *heap);
void printHeap(Heap *heap);
void resetGcStats(void);
void printGcStats(void);

void heapAddMutator(Heap *heap, struct Thread *thread);
void heapEndMutator(Heap *heap, struct Thread *thread);
// Retire every mutator's TLAB tail into a free chunk so a heap walk sees no
// half-carved space. Called with the world stopped.
void heapFillAllTlabTails(Heap *heap);

void heapGcPoll(Heap *heap, struct Thread *self);
void heapGcBegin(Heap *heap, struct Thread *self);
void heapGcEnd(Heap *heap, struct Thread *self);
void heapGcEnterBlocked(Heap *heap, struct Thread *self);
void heapGcLeaveBlocked(Heap *heap, struct Thread *self);

void heapCodegenLockEnter(Heap *heap);
void heapCodegenLockLeave(Heap *heap);
void heapSymbolLockEnter(Heap *heap);
void heapSymbolLockLeave(Heap *heap);
size_t heapMonitorEnterStripe(Heap *heap, RawObject *object);
void heapMonitorExitStripe(Heap *heap, size_t stripe);

#endif
