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
	// Every OS thread that mutates THIS heap links itself here (via Thread.nextMutator)
	// so the GC can scan the roots of all of them, not just the collecting thread.
	// One entry today (the owner); several once worker threads share the heap.
	struct Thread *mutators;
} Heap;

void heapAddMutator(Heap *heap, struct Thread *thread);

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
