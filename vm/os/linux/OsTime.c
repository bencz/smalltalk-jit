// Linux time services (vm/os/Os.h).
#include "os/Os.h"
#include "core/Assert.h"
#include <sys/time.h>
#include <time.h>


int64_t osCurrentMicroTime(void)
{
	struct timeval time;
	int result = gettimeofday(&time, NULL);
	if (result != 0) {
		FAIL();
	}
	return time.tv_sec * 1000000 + time.tv_usec;
}


int64_t osMonotonicNanos(void)
{
	struct timespec time;
	int result = clock_gettime(CLOCK_MONOTONIC, &time);
	if (result != 0) {
		FAIL();
	}
	return (int64_t) time.tv_sec * 1000000000 + time.tv_nsec;
}


int64_t osLocalUtcOffsetSeconds(int64_t epochSeconds)
{
	time_t instant = (time_t) epochSeconds;
	struct tm local;
	if (localtime_r(&instant, &local) == NULL) {
		return 0;
	}
	return local.tm_gmtoff;
}
