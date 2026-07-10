#include "Thread.h"
#include "StackFrame.h"
#include "Heap.h"
#include "Handle.h"
#include "Assert.h"

__thread Thread CurrentThread = { 0 };


void initThread(Thread *thread)
{
	initHeap(&thread->heap, thread);
	// Start with an empty TLAB (top == end) pointing at the fresh nursery top, so
	// the first allocation takes the refill path and carves a real chunk.
	thread->tlab.top = thread->heap.newSpace.top;
	thread->tlab.end = thread->heap.newSpace.top;
	thread->stackFramesTail = NULL;
}


void initThreadContext(Thread *thread)
{
	if (thread->context == 0) {
		RawContext *context = (RawContext *) allocateObject(&CurrentThread.heap, Handles.MethodContext->raw, 0);
		context->thread = thread;
		thread->context = tagPtr(context);
	}
}


void freeThread(Thread *thread)
{
	thread->context = 0;
	freeHeap(&thread->heap);
}


void threadSetExitFrame(StackFrame *stackFrame)
{
	CurrentThread.stackFramesTail->exit = stackFrame;
}
