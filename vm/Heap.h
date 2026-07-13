#ifndef HEAP_H
#define HEAP_H

#include "Object.h"
#include "HeapPage.h"
#include "Scavenger.h"
#include "RememberedSet.h"
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
#define STUB_COUNT 4

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
	// Every OS thread that mutates THIS heap links itself here (via Thread.nextMutator)
	// so the GC can scan the roots of all of them, not just the collecting thread.
	// One entry today (the owner); several once worker threads share the heap.
	struct Thread *mutators;
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
void heapGcEnterBlocked(Heap *heap, struct Thread *self);
void heapGcLeaveBlocked(Heap *heap, struct Thread *self);

void initHeap(Heap *heap, struct Thread *thread);
void freeHeap(Heap *heap);
RawObject *allocateObject(Heap *heap, RawClass *class, size_t size);
void freeObject(PageSpace *space, RawObject *object);
struct NativeCode *allocateNativeCode(Heap *heap, size_t size, size_t pointersOffsetsSize);
uint8_t *allocate(Heap *heap, size_t size);
uint8_t *tryAllocateOld(Heap *heap, size_t size, _Bool grow);
void collectGarbage(struct Thread *thread);
void markAndSweep(struct Thread *thread);
void verifyHeap(Heap *heap);
void printHeap(Heap *heap);
int tlabConcurrencySelfTest(void); // ST_TLAB_TEST=1 ./st

#endif
