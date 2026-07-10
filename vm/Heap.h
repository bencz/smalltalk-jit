#ifndef HEAP_H
#define HEAP_H

#include "Object.h"
#include "HeapPage.h"
#include "Scavenger.h"
#include "RememberedSet.h"
#include <pthread.h>

struct Thread;
struct NativeCode;

typedef struct Heap {
	struct Thread *thread;
	Scavenger newSpace;
	PageSpace oldSpace;
	PageSpace execSpace;
	size_t oldGcThreshold; // run a full GC only once old space grows past this
	// Guards carving TLAB chunks out of the shared young space: the per-mutator
	// bump inside a TLAB stays lock-free; only the (rare) refill takes this lock,
	// so several worker OS threads can share one nursery.
	pthread_mutex_t youngLock;
	// Guards the old space (free list + page growth). Promotion during a
	// (stop-the-world) GC is uncontended; this covers concurrent mutators
	// allocating large/old objects, and future concurrent promotion.
	pthread_mutex_t oldLock;
	// Every OS thread that mutates THIS heap links itself here (via Thread.nextMutator)
	// so the GC can scan the roots of all of them, not just the collecting thread.
	// One entry today (the owner); several once worker threads share the heap.
	struct Thread *mutators;
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
// Stop-the-world coordination (shared heap). `self` is excluded from the wait.
void heapGcPoll(Heap *heap, struct Thread *self);
void heapGcBegin(Heap *heap, struct Thread *self);
void heapGcEnd(Heap *heap);
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
