#include "runtime/Stream.h"
#include "core/Thread.h"
#include "core/Handle.h"
#include "memory/Heap.h"
#include "core/Assert.h"
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

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


int streamOpen(RawString *fileName, intptr_t mode)
{
	HandleScope scope;
	openHandleScope(&scope);
	String *fileNameHandle = scopeHandle(fileName);
	char space[256];
	char *buffer = space;

	if (fileName->size > 256) {
		String *tmpString = (String *) copyResizedObject((Object *) fileNameHandle, fileName->size + 1);
		buffer = tmpString->raw->contents;
		buffer[fileName->size] = '\0';
	} else {
		stringPrintOn(fileNameHandle, buffer);
	}

	closeHandleScope(&scope, NULL);

	int openMode = 0;
	switch (mode) {
	case 1:
		openMode = O_RDONLY;
		break;
	case 1 << 1:
		// write: opens for writing and CREATES/truncates the target, so writing
		// to a brand-new file works like the standard "w" fopen mode.
		openMode = O_WRONLY | O_CREAT | O_TRUNC;
		break;
	case 1 << 2:
		// readOrWrite: opens read/write and CREATES the target if missing, but
		// does NOT truncate, so existing contents can be read back and updated
		// in place (standard "r+"/create semantics).
		openMode = O_RDWR | O_CREAT;
		break;
	default:
		return -1;
	}

	// The 0666 mode argument is honored only when O_CREAT is set (and is further
	// filtered by the process umask); it is ignored for a pure O_RDONLY open.
	return TEMP_FAILURE_RETRY(open(buffer, openMode, 0666));
}


_Bool streamClose(int descriptor)
{
	return close(descriptor) == 0;
}


ptrdiff_t streamRead(int descriptor, void *buffer, size_t size)
{
	return TEMP_FAILURE_RETRY(read(descriptor, buffer, size));
}


ptrdiff_t streamWrite(int descriptor, void *buffer, size_t size)
{
	return TEMP_FAILURE_RETRY(write(descriptor, buffer, size));
}


_Bool streamFlush(int descriptor)
{
	return TEMP_FAILURE_RETRY(fsync(descriptor)) == 0;
}


/*_Bool streamAtEnd(RawFileStream *stream)
{
	return feof(stream->file);
}*/


ptrdiff_t streamGetPosition(int descriptor)
{
	return TEMP_FAILURE_RETRY(lseek(descriptor, 0, SEEK_CUR));
}


_Bool streamSetPosition(int descriptor, ptrdiff_t position)
{
	return TEMP_FAILURE_RETRY(lseek(descriptor, position, SEEK_SET)) != -1;
}


intptr_t streamAvailable(int descriptor)
{
	int available;
	int result = TEMP_FAILURE_RETRY(ioctl(descriptor, FIONREAD, &available));
	if (result < 0) {
		return result;
	}
	return available;
}


IoError *getLastIoError(void)
{
	HandleScope scope;
	openHandleScope(&scope);

	char msg[256] = "IoError: ";
	strerror_r(errno, msg + 9, 256 - 9);
	IoError *error = newObject(Handles.IoError, 0);
	objectStorePtr((Object *) error,  &error->raw->messageText, (Object *) asString(msg));

	return closeHandleScope(&scope, error);
}
