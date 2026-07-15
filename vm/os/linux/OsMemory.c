// Linux virtual-memory services (vm/os/Os.h): heap pages, growable fiber
// stacks, and the instruction-cache publish hook.
#include "os/Os.h"
#include <sys/mman.h>
#include <unistd.h>


long osPageSize(void)
{
	static long cached = 0;
	if (cached == 0) {
		long pg = sysconf(_SC_PAGESIZE);
		cached = pg > 0 ? pg : 4096;
	}
	return cached;
}


void *osPageAlloc(size_t size, _Bool executable)
{
	int protection = PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0);
	void *p = mmap(NULL, size, protection, MAP_ANON | MAP_PRIVATE, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}


void *osPageReserve(size_t size)
{
	void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}


_Bool osPageCommit(void *addr, size_t size)
{
	return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
}


void osPageRelease(void *addr, size_t size)
{
	madvise(addr, size, MADV_DONTNEED);
}


_Bool osPageFree(void *addr, size_t size)
{
	return munmap(addr, size) == 0;
}


void osFlushICache(void *start, size_t size)
{
	// Compiles to nothing on x86-64 (coherent I-cache); emits the right
	// barriers/cache ops on arches that need them.
	__builtin___clear_cache((char *) start, (char *) start + size);
}
