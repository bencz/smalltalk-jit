// Linux time services (vm/os/Os.h).
#include "os/Os.h"
#include "core/Assert.h"
#include <sys/time.h>
#include <time.h>
#include <errno.h>


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


void osSleepNanos(int64_t nanos)
{
	if (nanos <= 0) {
		return;
	}
	struct timespec requested;
	requested.tv_sec = (time_t) (nanos / 1000000000);
	requested.tv_nsec = (long) (nanos % 1000000000);
	// LOOPING on EINTR with the REMAINING time, which is what nanosleep fills
	// in: restarting with the original duration would make a signal extend the
	// sleep instead of leaving it alone.
	struct timespec remaining;
	while (nanosleep(&requested, &remaining) != 0 && errno == EINTR) {
		requested = remaining;
	}
}
