// Linux entropy service (vm/os/Os.h): getrandom(2), the no-fd path to the
// kernel CSPRNG (glibc >= 2.25). Blocks only before the pool's first
// initialization, which is over by the time userland runs.
#include "os/Os.h"
#include <sys/random.h>
#include <errno.h>
#include <stdint.h>


_Bool osRandomBytes(void *buffer, size_t size)
{
	uint8_t *cursor = buffer;
	while (size > 0) {
		ssize_t got = getrandom(cursor, size, 0);
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			return 0;
		}
		cursor += got;
		size -= (size_t) got;
	}
	return 1;
}
