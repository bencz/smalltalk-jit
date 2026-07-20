#ifndef OS_FILE_H
#define OS_FILE_H

#include "Os.h"

// The file-I/O slice of the OS seam, 1:1 with vm/runtime/Stream.c and the
// perf-map emitter. BLOCKING semantics (file I/O does not park fibers);
// EINTR is retried INSIDE the layer, so callers never see a spurious
// failure. Errors: OsFd answers OS_FD_INVALID, the int64_t calls -1, the
// _Bool calls 0 — details via osLastError()/osErrorMessage().

typedef enum {
	OS_FILE_READ,              // read only, fail when missing         ("r")
	OS_FILE_WRITE_TRUNC,       // write only, create + truncate        ("w")
	OS_FILE_READ_WRITE_CREATE, // read/write, create, NO truncate      ("r+"/create)
	OS_FILE_APPEND_TRUNC       // write only, create + truncate, every
	                           // write lands atomically at end-of-file
	                           // (the perf-map multi-writer contract)
} OsFileMode;

OsFd osFileOpen(const char *path, OsFileMode mode);
_Bool osFileClose(OsFd fd);
int64_t osFileRead(OsFd fd, void *buffer, size_t size);
int64_t osFileWrite(OsFd fd, const void *buffer, size_t size);
_Bool osFileFlush(OsFd fd);
int64_t osFileGetPosition(OsFd fd);
_Bool osFileSetPosition(OsFd fd, int64_t position);

// Bytes readable without blocking (FIONREAD); -1 when the platform or the
// fd kind cannot tell.
int64_t osFileAvailable(OsFd fd);

#endif
