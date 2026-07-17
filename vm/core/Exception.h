#ifndef EXCEPTION_H
#define EXCEPTION_H

#include "core/Object.h"
#include "core/Thread.h"
#include "core/StackFrame.h"

typedef struct {
	OBJECT_HEADER;
	uint8_t *ip;
	Value parent;
	Value context;
} RawExceptionHandler;
OBJECT_HANDLE(ExceptionHandler);

// One pending ensure:/ifCurtailed: registration (see BlockUnwindPrimitive). The
// cleanup block is stored IN the handler (not fetched from the frame) so the
// terminate path can run cleanups of a fiber whose stack is already condemned.
typedef struct {
	OBJECT_HEADER;
	Value parent;
	Value context;
	Value block;
} RawUnwindHandler;
OBJECT_HANDLE(UnwindHandler);

// Head of the running fiber's on:do: handler chain. Now a per-mutator field (see Thread.h)
// rather than a standalone TLS global, so JIT-generated code reaches it via CTX->thread and
// every worker sees ITS OWN chain. All existing `CurrentExceptionHandler` / `&CurrentExceptionHandler`
// uses keep working through this alias.
#define CurrentExceptionHandler (CurrentThread.exceptionHandler)

Value  unwindExceptionHandler(RawObject *exception);

// Run pending ensure:/ifCurtailed: cleanups whose frames lie below `targetFrame`
// (both ports grow the stack down: a dying frame has a LOWER address than the
// frame the unwind lands in). Innermost-first; each entry is unlinked before its
// block runs. Called by the exception unwinder and, via nlrRunUnwindHandlers,
// by the non-local-return path.
void runUnwindHandlersBelow(uint8_t *targetFrame);

// Run the current fiber's WHOLE pending chain (self-terminate).
void runAllUnwindHandlers(void);

// Run a chain captured from a fiber that will never resume (cross-fiber
// terminate). Executes on the calling fiber; does not touch the calling
// fiber's own chain.
void runUnwindHandlerChain(Value head);

// Called from JIT code on the non-local-return slow path (pending unwind
// handlers exist): runs the cleanups below the home frame and answers the
// (possibly GC-moved) return value.
Value nlrRunUnwindHandlers(Value result, uint8_t *homeFrame);

// A stack cut (exception jump / non-local return) that crosses C frames must
// drop the per-thread records living in the region being cut: nested
// EntryStackFrames and HandleScopes are stack-allocated, so anything below
// `targetFrame` is about to die. Call AFTER the cleanups ran (during them the
// dying frames must remain walkable for the GC).
void unwindThreadStateTo(uint8_t *targetFrame);

#endif
