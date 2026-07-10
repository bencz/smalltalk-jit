#include "Thread.h"
#include "StackFrame.h"
#include "Heap.h"
#include "Handle.h"
#include "Assert.h"
#include <stdlib.h>

__thread Thread CurrentThread = { 0 };


void initThread(Thread *thread)
{
	// The heap is heap-allocated (not embedded) so that, in the multicore model,
	// several worker OS threads of one isolate can point their `heap` at the SAME
	// Heap. Each thread still owns its TLAB and roots.
	thread->heap = malloc(sizeof(Heap));
	initHeap(thread->heap, thread);
	initRememberedSet(&thread->rememberedSet);
	thread->nextMutator = NULL;
	thread->schedFibers = NULL; // set by schedulerInit if this thread runs a scheduler
	thread->schedFiberSlots = NULL;
	thread->schedCurrent = NULL;
	heapAddMutator(thread->heap, thread); // this thread now mutates (and roots) its heap
	// Start with an empty TLAB (top == end) pointing at the fresh nursery top, so
	// the first allocation takes the refill path and carves a real chunk.
	thread->tlab.top = thread->heap->newSpace.top;
	thread->tlab.end = thread->heap->newSpace.top;
	thread->stackFramesTail = NULL;
}


void initThreadContext(Thread *thread)
{
	if (thread->context == 0) {
		RawContext *context = (RawContext *) allocateObject(CurrentThread.heap, Handles.MethodContext->raw, 0);
		context->thread = thread;
		thread->context = tagPtr(context);
	}
}


void freeThread(Thread *thread)
{
	thread->context = 0;
	freeHeap(thread->heap);
	free(thread->heap);
	thread->heap = NULL;
}


void threadSetExitFrame(StackFrame *stackFrame)
{
	CurrentThread.stackFramesTail->exit = stackFrame;
}
