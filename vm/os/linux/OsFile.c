// Linux/POSIX implementation of the OsFile contract (vm/os/OsFile.h).
// Blocking file I/O with EINTR absorbed here, so the VM never sees a
// spurious -1/EINTR from a file operation.
#include "os/OsFile.h"
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#if !defined(TEMP_FAILURE_RETRY)
	static inline int64_t temp_failure_retry(int64_t expression) {
		int64_t __result;
		do {
			__result = expression;
		} while (__result == -1L && errno == EINTR);
		return __result;
	}
	#define TEMP_FAILURE_RETRY(expression) temp_failure_retry(expression)
#endif


OsFd osFileOpen(const char *path, OsFileMode mode)
{
	int openMode;
	switch (mode) {
	case OS_FILE_READ:
		openMode = O_RDONLY;
		break;
	case OS_FILE_WRITE_TRUNC:
		openMode = O_WRONLY | O_CREAT | O_TRUNC;
		break;
	case OS_FILE_READ_WRITE_CREATE:
		openMode = O_RDWR | O_CREAT;
		break;
	case OS_FILE_APPEND_TRUNC:
		openMode = O_WRONLY | O_CREAT | O_APPEND | O_TRUNC;
		break;
	default:
		return OS_FD_INVALID;
	}
	// 0666/0644 is honored only when O_CREAT is set, further filtered by the
	// process umask; ignored for a pure O_RDONLY open.
	int permissions = mode == OS_FILE_APPEND_TRUNC ? 0644 : 0666;
	int fd = (int) TEMP_FAILURE_RETRY(open(path, openMode, permissions));
	return fd < 0 ? OS_FD_INVALID : (OsFd) fd;
}


_Bool osFileClose(OsFd fd)
{
	return close((int) fd) == 0;
}


int64_t osFileRead(OsFd fd, void *buffer, size_t size)
{
	return TEMP_FAILURE_RETRY(read((int) fd, buffer, size));
}


int64_t osFileWrite(OsFd fd, const void *buffer, size_t size)
{
	return TEMP_FAILURE_RETRY(write((int) fd, buffer, size));
}


_Bool osFileFlush(OsFd fd)
{
	return TEMP_FAILURE_RETRY(fsync((int) fd)) == 0;
}


int64_t osFileGetPosition(OsFd fd)
{
	return TEMP_FAILURE_RETRY(lseek((int) fd, 0, SEEK_CUR));
}


_Bool osFileSetPosition(OsFd fd, int64_t position)
{
	return TEMP_FAILURE_RETRY(lseek((int) fd, position, SEEK_SET)) != -1;
}


int64_t osFileAvailable(OsFd fd)
{
	int available;
	if (TEMP_FAILURE_RETRY(ioctl((int) fd, FIONREAD, &available)) < 0) {
		return -1;
	}
	return available;
}


// ---------------------------------------------------------------------------
// Paths and directories
// ---------------------------------------------------------------------------

_Bool osPathInfo(const char *path, OsPathInfo *info)
{
	struct stat status;
	if (stat(path, &status) != 0) {
		return 0;
	}
	info->kind = S_ISREG(status.st_mode) ? OS_PATH_FILE
		: S_ISDIR(status.st_mode) ? OS_PATH_DIRECTORY : OS_PATH_OTHER;
	info->sizeInBytes = (int64_t) status.st_size;
	// MILLISECONDS, and the same derivation tools/Project.h uses for its
	// staleness probe. The two have to agree exactly: one writes build.deps and
	// the other compares against it, and a rounding difference between them
	// would make a project either permanently stale or permanently fresh.
	info->modifiedMillis = (int64_t) status.st_mtim.tv_sec * 1000
		+ status.st_mtim.tv_nsec / 1000000;
	return 1;
}


// 0777 filtered by the umask, which is what the shell's `mkdir` does too.
_Bool osPathCreateDirectory(const char *path)
{
	return mkdir(path, 0777) == 0;
}


_Bool osPathRemoveDirectory(const char *path)
{
	return rmdir(path) == 0;
}


_Bool osPathRemoveFile(const char *path)
{
	return unlink(path) == 0;
}


_Bool osPathRename(const char *from, const char *to)
{
	return rename(from, to) == 0;
}


_Bool osPathSetCurrentDirectory(const char *path)
{
	return chdir(path) == 0;
}


_Bool osPathCurrentDirectory(char *buffer, size_t size)
{
	return getcwd(buffer, size) != NULL;
}


_Bool osPathCanonical(const char *path, char *buffer, size_t size)
{
	// realpath(path, NULL) MALLOCS the answer, and that is the form used here
	// on purpose: the two-argument form requires the buffer to be at least
	// PATH_MAX and silently overflows a smaller one, which is a stack smash
	// driven by a filename. This way the length is known before anything is
	// copied, and a path too long for the caller is a clean 0.
	char *resolved = realpath(path, NULL);
	if (resolved == NULL) {
		return 0;
	}
	size_t length = strlen(resolved);
	if (length + 1 > size) {
		free(resolved);
		return 0;
	}
	memcpy(buffer, resolved, length + 1);
	free(resolved);
	return 1;
}


struct OsDirectory {
	DIR *handle;
};


OsDirectory *osDirectoryOpen(const char *path)
{
	DIR *handle = opendir(path);
	if (handle == NULL) {
		return NULL;
	}
	OsDirectory *directory = malloc(sizeof *directory);
	if (directory == NULL) {
		closedir(handle);
		return NULL;
	}
	directory->handle = handle;
	return directory;
}


_Bool osDirectoryNext(OsDirectory *directory, char *name, size_t size)
{
	if (directory == NULL) {
		return 0;
	}
	for (;;) {
		// errno is NOT reset and the result not distinguished from a real error:
		// readdir answers NULL for both end-of-directory and failure, and the
		// caller has nothing useful to do differently. Both mean "no more".
		struct dirent *entry = readdir(directory->handle);
		if (entry == NULL) {
			return 0;
		}
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		size_t length = strlen(entry->d_name);
		if (length + 1 > size) {
			continue; // skipped, never truncated: a truncated name is another file
		}
		memcpy(name, entry->d_name, length + 1);
		return 1;
	}
}


void osDirectoryClose(OsDirectory *directory)
{
	if (directory == NULL) {
		return;
	}
	closedir(directory->handle);
	free(directory);
}
