// File-stream support over the OS seam (vm/os/OsFile.h): this layer only
// converts between Smalltalk values (mode bits, String file names) and the
// OsFile contract. Blocking semantics; EINTR is absorbed inside the OS layer.
#include "runtime/Stream.h"
#include "core/Thread.h"
#include "core/Handle.h"
#include "memory/Heap.h"
#include "core/Assert.h"
#include "os/OsFile.h"
#include <string.h>


OsFd streamOpen(RawString *fileName, intptr_t mode)
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

	OsFileMode fileMode;
	switch (mode) {
	case 1:
		fileMode = OS_FILE_READ;
		break;
	case 1 << 1:
		// write: opens for writing and CREATES/truncates the target, so writing
		// to a brand-new file works like the standard "w" fopen mode.
		fileMode = OS_FILE_WRITE_TRUNC;
		break;
	case 1 << 2:
		// readOrWrite: opens read/write and CREATES the target if missing, but
		// does NOT truncate, so existing contents can be read back and updated
		// in place (standard "r+"/create semantics).
		fileMode = OS_FILE_READ_WRITE_CREATE;
		break;
	default:
		return OS_FD_INVALID;
	}

	return osFileOpen(buffer, fileMode);
}


_Bool streamClose(OsFd descriptor)
{
	return osFileClose(descriptor);
}


ptrdiff_t streamRead(OsFd descriptor, void *buffer, size_t size)
{
	return (ptrdiff_t) osFileRead(descriptor, buffer, size);
}


ptrdiff_t streamWrite(OsFd descriptor, void *buffer, size_t size)
{
	return (ptrdiff_t) osFileWrite(descriptor, buffer, size);
}


_Bool streamFlush(OsFd descriptor)
{
	return osFileFlush(descriptor);
}


ptrdiff_t streamGetPosition(OsFd descriptor)
{
	return (ptrdiff_t) osFileGetPosition(descriptor);
}


_Bool streamSetPosition(OsFd descriptor, ptrdiff_t position)
{
	return osFileSetPosition(descriptor, position);
}


intptr_t streamAvailable(OsFd descriptor)
{
	return (intptr_t) osFileAvailable(descriptor);
}


IoError *getLastIoError(void)
{
	HandleScope scope;
	openHandleScope(&scope);

	char msg[256] = "IoError: ";
	osErrorMessage(osLastError(), msg + 9, sizeof(msg) - 9);
	IoError *error = newObject(Handles.IoError, 0);
	objectStorePtr((Object *) error,  &error->raw->messageText, (Object *) asString(msg));

	return closeHandleScope(&scope, error);
}