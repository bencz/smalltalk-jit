#ifndef FILE_STREAM_H
#define FILE_STREAM_H

#include "core/Object.h"
#include "runtime/String.h"
#include "os/Os.h"

#define EXTERNAL_STREAM_BODY \
	OBJECT_HEADER; \
	Value buffer; \
	Value position; \
	Value buffered; \
	Value atEnd; \
	Value descriptor

typedef struct {
	EXTERNAL_STREAM_BODY;
} RawExternalStream;
OBJECT_HANDLE(ExternalStream);

typedef struct {
	EXTERNAL_STREAM_BODY;
	Value name;
} RawFileStream;
OBJECT_HANDLE(FileStream);

typedef struct {
	OBJECT_HEADER;
	Value messageText;
} RawIoError;
OBJECT_HANDLE(IoError);

OsFd streamOpen(RawString *fileName, intptr_t mode);
_Bool streamClose(OsFd descriptor);
ptrdiff_t streamRead(OsFd descriptor, void *buffer, size_t size);
ptrdiff_t streamWrite(OsFd descriptor, void *buffer, size_t size);
_Bool streamFlush(OsFd descriptor);
ptrdiff_t streamGetPosition(OsFd descriptor);
_Bool streamSetPosition(OsFd descriptor, ptrdiff_t position);
intptr_t streamAvailable(OsFd descriptor);
IoError *getLastIoError(void);

#endif

