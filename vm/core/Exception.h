#ifndef EXCEPTION_H
#define EXCEPTION_H

#include "core/Object.h"
#include "core/Thread.h"

typedef struct {
	OBJECT_HEADER;
	uint8_t *ip;
	Value parent;
	Value context;
} RawExceptionHandler;
OBJECT_HANDLE(ExceptionHandler);

// Head of the running fiber's on:do: handler chain. Now a per-mutator field (see Thread.h)
// rather than a standalone TLS global, so JIT-generated code reaches it via CTX->thread and
// every worker sees ITS OWN chain. All existing `CurrentExceptionHandler` / `&CurrentExceptionHandler`
// uses keep working through this alias.
#define CurrentExceptionHandler (CurrentThread.exceptionHandler)

Value  unwindExceptionHandler(RawObject *exception);

#endif
