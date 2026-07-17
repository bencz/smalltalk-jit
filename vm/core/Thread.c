#include "core/Thread.h"
#include "jit/TargetTraits.h"
#include "core/Lookup.h"
#include "core/StackFrame.h"
#include "memory/Heap.h"
#include "core/Handle.h"
#include "core/Exception.h"
#include "core/Assert.h"
#include <stdlib.h>

__thread Thread CurrentThread = { 0 };
ptrdiff_t gCurrentThreadTpoff = 0;
ptrdiff_t gLookupCacheTpoff = 0;


void initThread(Thread *thread)
{
	// PORT_ME(tls): the tpoff computation via targetThreadPointer() (per-arch
	// trait) is uniform, but each backend must emit its own TP-relative load (x64: %fs
	// prefix in asmLoadTls; aarch64: tpidr_el0; riscv: tp; ppc64: r13 with the
	// 0x7000 TP bias baked into the offset semantics — re-derive per ABI).
	// Offset of &CurrentThread from the thread pointer (same on every thread, initial-exec).
	// Computed here (before any JIT codegen); asmLoadTls bakes it so shared JIT code reaches
	// each running worker's own CurrentThread via %fs.
	gCurrentThreadTpoff = (char *) &CurrentThread - (char *) targetThreadPointer();
	// Same idea for the per-thread lookup cache: the JIT probes it via %fs:tpoff so
	// every worker uses its OWN cache (see Lookup.h for why baking &LookupCache races).
	gLookupCacheTpoff = (char *) &LookupCache - (char *) targetThreadPointer();
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
	thread->exceptionHandler = 0; // empty on:do: chain
	thread->schedExceptionHandler = &thread->exceptionHandler; // this thread's own handler-chain slot
	thread->unwindHandler = 0; // empty ensure:/ifCurtailed: chain
	thread->schedUnwindHandler = &thread->unwindHandler;
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
