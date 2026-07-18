#include "core/Exception.h"
#include "core/Smalltalk.h"
#include "core/Class.h"
#include "core/Entry.h"
#include "core/Handle.h"
#include "core/StackFrame.h"

Value findExceptionHandler(RawObject *rawException)
{
	if (CurrentExceptionHandler == 0) {
		return 0;
	}

	HandleScope scope;
	openHandleScope(&scope);

	Object *exception = scopeHandle(rawException);
	ExceptionHandler *handler = scopeHandle(asObject(CurrentExceptionHandler));
	Object *originalHead = scopeHandle(asObject(CurrentExceptionHandler));

	do {
		RawContext *context = (RawContext *) asObject(handler->raw->context);
		if (contextHasValidFrame(context)) {
			Object *exceptionClass = scopeHandle(asObject(stackFrameGetArg(context->frame, 1)));
			EntryArgs args = { .size = 0 };
			entryArgsAddObject(&args, exceptionClass);
			entryArgsAddObject(&args, exception);
			// While handles: runs, exclude this candidate and everything inside
			// it, so a signal raised by handles: itself searches strictly
			// outward (the pre-M2 code zeroed the whole chain instead, which
			// also wiped it for the unhandled case).
			CurrentExceptionHandler = handler->raw->parent;
			_Bool handles = isTaggedTrue(sendMessage(Handles.handlesSymbol, &args));
			if (handles) {
				// Leave the head at the matched handler's parent: the handler
				// is now in progress. NO unwinding here (milestone 2): the
				// signal primitive runs the handler on top of the signaling
				// frames; cleanups run only if and when the handler decides
				// to unwind (normal completion / return: / retry).
				Value found = getTaggedPtr(handler);
				closeHandleScope(&scope, NULL);
				return found;
			}
			CurrentExceptionHandler = getTaggedPtr(originalHead);
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


Value unwindReturning(Value result, uint8_t *targetFrame)
{
	HandleScope scope;
	openHandleScope(&scope);

	// The carried result must survive GCs triggered by the cleanups.
	Object *resultHandle = NULL;
	if (valueTypeOf(result, VALUE_POINTER)) {
		resultHandle = scopeHandle(asObject(result));
	}

	// Non-local return: the home frame itself dies too, but ensure frames are
	// always strict callees of it, so `below target` covers all. Handler
	// completion: the on:do: frame survives; everything the cut abandons
	// (protected-block frames, signal frames, handler frames) is below it.
	runUnwindHandlersBelow(targetFrame);

	if (resultHandle != NULL) {
		result = getTaggedPtr(resultHandle);
	}
	closeHandleScope(&scope, NULL);
	unwindThreadStateTo(targetFrame);
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
		// The scope is abandoned without closeHandleScope: release its heap
		// overflow chunks here or they leak.
		handleScopeFreeChunks(CurrentThread.handleScopes);
		CurrentThread.handleScopes = CurrentThread.handleScopes->parent;
	}
}
