// Linux process introspection (vm/os/Os.h): the executable path comes from
// the /proc/self/exe symlink, which the kernel keeps pointing at the running
// binary even after a rename. readlink(2) does not NUL-terminate and gives no
// truncation signal beyond filling the buffer, so a result that used the whole
// buffer is treated as "does not fit".
#include "os/Os.h"
#include <unistd.h>


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
