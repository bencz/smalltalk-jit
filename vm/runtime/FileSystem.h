#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include "core/Object.h"
#include "runtime/Collection.h"

// POSIX file-system calls behind the File* primitives (Primitives.c). Plain
// syscall wrappers on C strings; the one allocating entry point (fsListDir)
// builds its result with the barrier-safe collection helpers. Paths longer
// than FS_PATH_MAX - 1 bytes are rejected at the marshalling layer.

#define FS_PATH_MAX 4096

// fsStatPath fills info[3] as {type, size, mtimeMillis} where type is
// 1 = regular file, 2 = directory, 3 = anything else. Follows symlinks.
_Bool fsStatPath(const char *path, int64_t info[3]);

_Bool fsMkdir(const char *path);
_Bool fsUnlink(const char *path);
_Bool fsRmdir(const char *path);
_Bool fsRename(const char *from, const char *to);
_Bool fsChdir(const char *path);
_Bool fsGetCwd(char *buffer, size_t capacity);
_Bool fsRealpath(const char *path, char *resolved);   // resolved: FS_PATH_MAX bytes

// Entry names of `path` (no "." / ".."), as an OrderedCollection of String.
// NULL if the directory cannot be read. Caller must hold an open HandleScope.
OrderedCollection *fsListDir(const char *path);

#endif
