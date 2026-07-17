#ifndef HEAP_H
#define HEAP_H

#include "core/Object.h"
#include "memory/HeapPage.h"
#include "memory/Scavenger.h"
#include "memory/RememberedSet.h"
#include <pthread.h>

struct Thread;
struct NativeCode;
struct Scheduler;
struct SmalltalkHandles;

// JIT stubs are shared per-HEAP, not per-OS-thread: all worker threads of one heap
// reuse the same generated stub bodies (they reach per-thread state via CTX->thread),
// so a stub is generated once per heap instead of once per worker. Kept in sync with
// the StubId enum in StubCode.h. Per-heap (not global) keeps isolates independent —
// each heap's stub `insts` live in that heap's execSpace.
#define STUB_COUNT 5

typedef struct Heap {
	struct Thread *thread;
	// Well-known handles (kernel classes, Smalltalk global dict, symbol table), shared
	// by every worker OS thread of this heap. Reached via the `Handles` macro
	// (Handle.h) as `*CurrentThread.heap->handles`. Allocated in initHeap, populated at
	// bootstrap. Per-heap (not TLS) so shared-memory workers see one set; separate
	// isolates keep separate handles via their separate heaps.
	struct SmalltalkHandles *handles;
	Scavenger newSpace;
	PageSpace oldSpace;
	PageSpace execSpace;
	struct NativeCode *stubCode[STUB_COUNT]; // generated JIT stubs, shared by this heap's mutators
	size_t oldGcThreshold; // run a full GC only once old space grows past this
	// Bumped once per young collection (under STW/gcLock). Mutators compare it
	// against their Thread.lookupCacheEpoch on resume and flush their TLS
	// LookupCache: a scavenge MOVES young classes/selectors, so stale cache
	// entries would otherwise dangle into from-space and, once that memory is
	// reused, produce false HITS that dispatch the wrong native code.
	size_t gcEpoch;
	// Guards carving TLAB chunks out of the shared young space: the per-mutator
	// bump inside a TLAB stays lock-free; only the (rare) refill takes this lock,
	// so several worker OS threads can share one nursery.
	pthread_mutex_t youngLock;
	// Guards the old space (free list + page growth). Promotion during a
	// (stop-the-world) GC is uncontended; this covers concurrent mutators
	// allocating large/old objects, and future concurrent promotion.
	pthread_mutex_t oldLock;
	// Guards the executable space (JIT code). Several worker OS threads lazily
	// generate methods into ONE shared exec space; without this lock their concurrent
	// pageSpaceAllocate calls corrupt the exec freelist/page list/index. (Stubs are
	// now generated once per heap — stubCode[] above — so only method compilation
	// still contends here.) Exec allocation never triggers a young/old GC, so holding
	// it cannot deadlock the safepoint handshake.
	pthread_mutex_t execLock;
	// Serializes JIT code generation across worker threads. Codegen allocates young
	// heap objects (assembler scratch, stackmaps) and can trigger a scavenge, so a
	// thread WAITING for this lock counts as safe (it enters the blocked state before
	// locking) or a peer collector would wait for it forever. Recursive use (a stub
	// generated while compiling a method) is handled by a per-thread depth counter.
	pthread_mutex_t codegenLock;
	// The ONE global synchronization monitor for the Smalltalk sync primitives
	// (Semaphore/Channel/Future/Mailbox/ActorSystem). Under the multi-worker pool a
	// fiber's "check a condition then park" is no longer atomic by virtue of cooperative
	// scheduling, so those primitives run their (short, FLAT — never nested) critical
	// sections under this monitor. A thread WAITING for it counts as GC-safe (same
	// enter-blocked discipline as codegenLock), and it is only ever taken monitor→
	// sched->lock (never the reverse), so it cannot deadlock the STW handshake. Coarse
	// by design (correctness first); sharding it per sync-object is a perf follow-up.
	pthread_mutex_t monitorLock;
	// Serializes symbol interning (asSymbol + growSymbolTable) across worker threads: the
	// symbol table is ONE shared open-addressed hash per heap, so concurrent probe/insert/
	// grow would corrupt it. Re-entrant (growSymbolTable re-enters asSymbol via setGlobal),
	// GC-safe acquisition (interning allocates the new Symbol / bigger table → may scavenge).
	// The occupancy counter is per-heap too (was per-thread TLS, wrong for a shared table).
	pthread_mutex_t symbolLock;
	size_t symbolCount;      // live entries in the symbol table (guarded by symbolLock)
	_Bool symbolCountValid;  // recomputed lazily on first intern (snapshot restores the table only)
	// Every OS thread that mutates THIS heap links itself here (via Thread.nextMutator)
	// so the GC can scan the roots of all of them, not just the collecting thread.
	// One entry today (the owner); several once worker threads share the heap.
	struct Thread *mutators;
	// The CONSOLIDATED old->young remembered set for this heap. Each scavenge/full-GC
	// re-adds every surviving old->young root here (not into the transient collector
	// thread), so the set survives worker-thread exit — a departing worker's per-thread
	// barrier delta is spliced in by heapEndMutator. Write barriers still append to the
	// PER-THREAD Thread.rememberedSet (lock-free); this heap-level set is touched ONLY
	// under gcLock / at a stop-the-world safepoint, never by a barrier.
	RememberedSet rememberedSet;
	// The fiber scheduler shared by all OS-thread workers of this heap (ready queue,
	// fiber registry, timer heap, epoll). Per-heap (not per-OS-thread TLS) so a pool
	// of workers can pull fibers from ONE queue over this one heap. NULL until
	// schedulerInit runs (bootstrap allocates before any fiber exists). Isolates keep
	// separate schedulers because they have separate heaps.
	struct Scheduler *sched;
	// --- stop-the-world GC coordination for a shared heap ---
	// gcLock: at most one collector at a time. safepoint{Lock,Cond,Requested}: the
	// handshake — a collector parks every other mutator (they poll at allocation
	// slow paths) before touching the object graph. Unused while single-mutator.
	pthread_mutex_t gcLock;
	pthread_mutex_t safepointLock;
	pthread_cond_t safepointCond;
	int safepointRequested;
} Heap;

void heapAddMutator(Heap *heap, struct Thread *thread);
void heapEndMutator(Heap *heap, struct Thread *thread); // unregister an exiting worker
// Stop-the-world coordination (shared heap). `self` is excluded from the wait.
void heapGcPoll(Heap *heap, struct Thread *self);
void heapGcBegin(Heap *heap, struct Thread *self);
void heapGcEnd(Heap *heap);
void heapCodegenLockEnter(Heap *heap); // serialize JIT codegen across worker threads
void heapCodegenLockLeave(Heap *heap);
void heapMonitorEnter(Heap *heap); // GC-safe acquire of the Smalltalk sync monitor
void heapMonitorExit(Heap *heap);
void heapSymbolLockEnter(Heap *heap); // re-entrant, GC-safe: serialize symbol interning
void heapSymbolLockLeave(Heap *heap);
void heapFillAllTlabTails(Heap *heap); // retire every mutator's TLAB tail (become: under STW)
void heapGcEnterBlocked(Heap *heap, struct Thread *self);
void heapGcLeaveBlocked(Heap *heap, struct Thread *self);

void initHeap(Heap *heap, struct Thread *thread);
void freeHeap(Heap *heap);
RawObject *allocateObject(Heap *heap, RawClass *class, size_t size);
void freeObject(PageSpace *space, RawObject *object);
struct NativeCode *allocateNativeCode(Heap *heap, size_t size, size_t pointersOffsetsSize, size_t icCellsSize);
uint8_t *allocate(Heap *heap, size_t size);
uint8_t *tryAllocateOld(Heap *heap, size_t size, _Bool grow);
void collectGarbage(struct Thread *thread);
void markAndSweep(struct Thread *thread);
void verifyHeap(Heap *heap);
void printHeap(Heap *heap);
int tlabConcurrencySelfTest(void); // ST_TLAB_TEST=1 ./st

#endif
