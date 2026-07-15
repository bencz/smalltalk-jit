// Linux time services (vm/os/Os.h).
#include "os/Os.h"
#include "core/Assert.h"
#include <sys/time.h>


int64_t osCurrentMicroTime(void)
{
	struct timeval time;
	int result = gettimeofday(&time, NULL);
	if (result != 0) {
		FAIL();
	}
	return time.tv_sec * 1000000 + time.tv_usec;
}
