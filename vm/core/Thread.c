#include "core/Thread.h"
#include "core/Assert.h"
#include "core/Handle.h"
#include "memory/Heap.h"
#include <stdlib.h>
#include <string.h>

__thread Thread CurrentThread = { 0 };
ptrdiff_t gCurrentThreadTpoff = 0;


// Bring one mutator up: its heap, its remembered set, its allocation buffer.
//
// The heap is heap-allocated rather than embedded because several worker threads
// of one isolate point their `heap` at the SAME Heap; what each thread owns is
// its TLAB, its remembered set and its roots.
//
// PORT_ME(tls): gCurrentThreadTpoff is the offset of &CurrentThread from the OS
// thread pointer, which generated code bakes once it reads thread state
// directly. Nothing in tier 1 does yet (the safepoint poll takes the address it
// needs as an operand), so it stays zero rather than being computed from a
// per-arch trait that v2 does not have yet.
void initThread(Thread *thread)
{
	memset(thread, 0, sizeof(*thread));
	thread->heap = malloc(sizeof(Heap));
	ASSERT(thread->heap != NULL);
	initHeap(thread->heap, thread);
	initRememberedSet(&thread->rememberedSet);
	heapAddMutator(thread->heap, thread);
	// An EMPTY buffer pointing at the fresh nursery top, so the first allocation
	// takes the refill path and carves a real chunk rather than handing out a
	// range nobody reserved.
	thread->tlab.top = thread->heap->newSpace.top;
	thread->tlab.end = thread->heap->newSpace.top;
}


void freeThread(Thread *thread)
{
	freeHeap(thread->heap);
	free(thread->heap);
	thread->heap = NULL;
}
