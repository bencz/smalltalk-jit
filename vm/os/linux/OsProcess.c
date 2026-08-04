// Linux process introspection (vm/os/Os.h): the executable path comes from
// the /proc/self/exe symlink, which the kernel keeps pointing at the running
// binary even after a rename. readlink(2) does not NUL-terminate and gives no
// truncation signal beyond filling the buffer, so a result that used the whole
// buffer is treated as "does not fit".
#include "os/Os.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>


_Bool osExecutablePath(char *buffer, size_t size)
{
	if (size < 2) {
		return 0;
	}
	ssize_t length = readlink("/proc/self/exe", buffer, size - 1);
	if (length <= 0 || (size_t) length >= size - 1) {
		return 0;
	}
	buffer[length] = '\0';
	return 1;
}


int osLastError(void)
{
	return errno;
}


void osErrorMessage(int code, char *buffer, size_t size)
{
	// The XSI strerror_r fills the buffer; on the GNU variant the result may
	// live in a static string instead, so route through the return value.
#if defined(_GNU_SOURCE) || (defined(__GLIBC__) && !((_POSIX_C_SOURCE >= 200112L) && !defined(_GNU_SOURCE)))
	char scratch[256];
	char *message = strerror_r(code, scratch, sizeof(scratch));
	snprintf(buffer, size, "%s", message);
#else
	if (strerror_r(code, buffer, size) != 0 && size > 0) {
		snprintf(buffer, size, "error %d", code);
	}
#endif
}


_Bool osJitMapPath(char *buffer, size_t size)
{
	return snprintf(buffer, size, "/tmp/perf-%d.map", (int) getpid()) < (int) size;
}


_Bool osEnvironmentValue(const char *name, char *buffer, size_t size)
{
	const char *value = getenv(name);
	if (value == NULL) {
		return 0;
	}
	size_t length = strlen(value);
	if (length + 1 > size) {
		return 0;
	}
	memcpy(buffer, value, length + 1);
	return 1;
}


_Bool osDebugBreak(void)
{
	// TracerPid is 0 unless something is ptrace-attached (gdb, strace). Read
	// fresh on every call: the debugger may have attached since the last one.
	FILE *status = fopen("/proc/self/status", "r");
	if (status == NULL) {
		return 0;
	}
	char line[128];
	_Bool traced = 0;
	while (fgets(line, sizeof line, status) != NULL) {
		if (strncmp(line, "TracerPid:", 10) == 0) {
			traced = strtol(line + 10, NULL, 10) != 0;
			break;
		}
	}
	fclose(status);
	if (!traced) {
		return 0;
	}
	raise(SIGTRAP);
	return 1;
}
