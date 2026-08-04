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


// ---------------------------------------------------------------------------
// Paths and directories
// ---------------------------------------------------------------------------
//
// The metadata half of the seam. It lives HERE, behind Os.h, and not in
// runtime/ the way the old VM had it: `stat`, `mkdir`, `opendir` and
// `realpath` are POSIX, and ADR 0009's rule about keeping platform detail on
// one side of a seam is not only about registers. A Windows port replaces this
// file and nothing above it.
//
// Nothing here allocates a Smalltalk object, which is the OTHER reason the old
// arrangement had to go: its directory lister answered an OrderedCollection, so
// the OS layer had to know the object model. Listing is an ITERATOR here, and
// the primitive builds whatever collection the kernel wants.

// The three numbers `statPath:` answers to the kernel, in this order. The
// meaning of `kind` is the kernel's (Files/File.st asks for 1, Directory.st
// asks for 2), so it is fixed here rather than passed through as st_mode.
typedef enum {
	OS_PATH_FILE = 1,
	OS_PATH_DIRECTORY = 2,
	OS_PATH_OTHER = 3       // a socket, a fifo, a device: exists, but neither
} OsPathKind;

typedef struct {
	OsPathKind kind;
	int64_t sizeInBytes;
	int64_t modifiedMillis;  // epoch milliseconds, the unit build.deps records
} OsPathInfo;

// 0 when the path does not exist or cannot be stat'd, and `info` is untouched.
_Bool osPathInfo(const char *path, OsPathInfo *info);

_Bool osPathCreateDirectory(const char *path);
_Bool osPathRemoveDirectory(const char *path);
_Bool osPathRemoveFile(const char *path);
_Bool osPathRename(const char *from, const char *to);
_Bool osPathSetCurrentDirectory(const char *path);

// Both write at most `size` bytes including the terminator, and answer 0 when
// the answer does not fit or the OS refuses it. osPathCanonical resolves
// symlinks and dot segments; the caller decides what to do when it cannot.
_Bool osPathCurrentDirectory(char *buffer, size_t size);
_Bool osPathCanonical(const char *path, char *buffer, size_t size);

// Directory iteration, as an iterator rather than a collection, so no OS file
// has to know the object model. `.` and `..` are SKIPPED here so every caller
// does not have to remember to.
//
//   OsDirectory *dir = osDirectoryOpen(path);
//   while (osDirectoryNext(dir, name, sizeof name)) { ... }
//   osDirectoryClose(dir);
//
// osDirectoryNext answers 0 at the end of the directory AND on a name too long
// for the buffer; a name that does not fit is skipped rather than truncated,
// because a truncated name is a path to the wrong file.
typedef struct OsDirectory OsDirectory;

OsDirectory *osDirectoryOpen(const char *path);
_Bool osDirectoryNext(OsDirectory *directory, char *name, size_t size);
void osDirectoryClose(OsDirectory *directory);

#endif
