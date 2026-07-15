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
				closeHandleScope(&scope, NULL);
				return getTaggedPtr(handler);
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
