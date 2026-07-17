#include "core/Exception.h"
#include "core/Smalltalk.h"
#include "core/Class.h"
#include "core/Entry.h"
#include "core/Handle.h"
#include "core/StackFrame.h"

Value unwindExceptionHandler(RawObject *rawException)
{
	if (CurrentExceptionHandler == 0) {
		return 0;
	}

	HandleScope scope;
	openHandleScope(&scope);

	Object *exception = scopeHandle(rawException);
	ExceptionHandler *handler = scopeHandle(asObject(CurrentExceptionHandler));
	CurrentExceptionHandler = 0;

	do {
		RawContext *context = (RawContext *) asObject(handler->raw->context);
		if (contextHasValidFrame(context)) {
			Object *exceptionClass = scopeHandle(asObject(stackFrameGetArg(context->frame, 1)));
			EntryArgs args = { .size = 0 };
			entryArgsAddObject(&args, exceptionClass);
			entryArgsAddObject(&args, exception);
			if (isTaggedTrue(sendMessage(Handles.handlesSymbol, &args))) {
				CurrentExceptionHandler = handler->raw->parent;
				// The handler has decided: run the pending ensure:/ifCurtailed:
				// cleanups of the frames the jump to the handler will cut. The
				// signaling frames stay walkable while the cleanups run (the
				// signal primitive still re-reads its frame after this returns);
				// the C-side records of the cut region (nested entries/scopes)
				// are dropped by the signal primitive AT the cut, via
				// unwindThreadStateTo, after its last read of a dying frame.
				runUnwindHandlersBelow((uint8_t *) ((RawContext *) asObject(handler->raw->context))->frame);
				Value found = getTaggedPtr(handler);
				closeHandleScope(&scope, NULL);
				return found;
			}
		}
		if (handler->raw->parent == 0) {
			closeHandleScope(&scope, NULL);
			return 0;
		}
		handler = scopeHandle(asObject(handler->raw->parent));
	} while (1);

	closeHandleScope(&scope, NULL);
	return 0;
}


// Evaluate one cleanup block (0-arg #value send). The block handle is scoped by
// the caller; a GC during the send updates it.
static void runCleanupBlock(Object *block)
{
	EntryArgs args = { .size = 0 };
	entryArgsAddObject(&args, block);
	sendMessage(Handles.valueSymbol, &args);
}


void runUnwindHandlersBelow(uint8_t *targetFrame)
{
	while (CurrentThread.unwindHandler != 0) {
		RawUnwindHandler *rawHandler = (RawUnwindHandler *) asObject(CurrentThread.unwindHandler);
		RawContext *context = (RawContext *) asObject(rawHandler->context);
		if ((uint8_t *) context->frame >= targetFrame) {
			break; // registered in a frame that survives this unwind
		}
		// Unlink BEFORE running: a signal/return/terminate inside the cleanup
		// must never re-run it. One scope per iteration (chains can be deep).
		CurrentThread.unwindHandler = rawHandler->parent;
		HandleScope scope;
		openHandleScope(&scope);
		runCleanupBlock(scopeHandle(asObject(rawHandler->block)));
		closeHandleScope(&scope, NULL);
	}
}


void runAllUnwindHandlers(void)
{
	runUnwindHandlersBelow((uint8_t *) UINTPTR_MAX);
}


void runUnwindHandlerChain(Value head)
{
	// Cross-fiber terminate: the chain belongs to a fiber that will never run
	// again, so there is no head slot to maintain; walk and run. The dead
	// fiber is already unregistered, so this walk's handles are the only roots
	// keeping the chain (and the cleanup blocks) alive.
	while (head != 0) {
		HandleScope scope;
		openHandleScope(&scope);
		UnwindHandler *handler = scopeHandle(asObject(head));
		runCleanupBlock(scopeHandle(asObject(handler->raw->block)));
		head = handler->raw->parent; // via the handle: valid after any GC in the send
		closeHandleScope(&scope, NULL);
	}
}


Value nlrRunUnwindHandlers(Value result, uint8_t *homeFrame)
{
	HandleScope scope;
	openHandleScope(&scope);

	// The non-local return value must survive GCs triggered by the cleanups.
	Object *resultHandle = NULL;
	if (valueTypeOf(result, VALUE_POINTER)) {
		resultHandle = scopeHandle(asObject(result));
	}

	// The home frame itself dies too (the return leaves the home method), but
	// ensure frames are always strict callees of it, so `below home` covers all.
	runUnwindHandlersBelow(homeFrame);

	if (resultHandle != NULL) {
		result = getTaggedPtr(resultHandle);
	}
	closeHandleScope(&scope, NULL);
	unwindThreadStateTo(homeFrame);
	return result;
}


void unwindThreadStateTo(uint8_t *targetFrame)
{
	// EntryStackFrame records and HandleScopes live on the (downward-growing)
	// stack; anything below the landing frame belongs to the region being cut.
	while (CurrentThread.stackFramesTail != NULL
			&& (uint8_t *) CurrentThread.stackFramesTail < targetFrame) {
		CurrentThread.stackFramesTail = CurrentThread.stackFramesTail->prev;
	}
	while (CurrentThread.handleScopes != NULL
			&& (uint8_t *) CurrentThread.handleScopes < targetFrame) {
		CurrentThread.handleScopes = CurrentThread.handleScopes->parent;
	}
}
