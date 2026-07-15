// Linux CPU topology / scheduling services (vm/os/Os.h).
#define _GNU_SOURCE // sched_getaffinity / CPU_COUNT
#include "os/Os.h"
#include <sched.h>
#include <unistd.h>


int osAvailableCoreCount(void)
{
#ifdef CPU_COUNT
	cpu_set_t set;
	if (sched_getaffinity(0, sizeof(set), &set) == 0) {
		int n = CPU_COUNT(&set);
		if (n >= 1) {
			return n;
		}
	}
#endif
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n >= 1 ? (int) n : 1;
}
