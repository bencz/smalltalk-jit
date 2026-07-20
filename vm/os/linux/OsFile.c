// Linux/POSIX implementation of the OsFile contract (vm/os/OsFile.h).
// Blocking file I/O with EINTR absorbed here, so the VM never sees a
// spurious -1/EINTR from a file operation.
#include "os/OsFile.h"
#include <sys/ioctl.h>
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
