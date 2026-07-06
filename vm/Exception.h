#ifndef EXCEPTION_H
#define EXCEPTION_H

#include "Object.h"
#include "Thread.h"

typedef struct {
	OBJECT_HEADER;
	uint8_t *ip;
	Value parent;
	Value context;
} RawExceptionHandler;
OBJECT_HANDLE(ExceptionHandler);

extern PER_ISOLATE Value CurrentExceptionHandler;

Value  unwindExceptionHandler(RawObject *exception);

#endif
