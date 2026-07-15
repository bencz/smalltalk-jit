#include "concurrency/Fiber.h"
#include "concurrency/Scheduler.h"
#include "core/Assert.h"
#include "os/Os.h"
#include <stdlib.h>
#include <stdint.h>

// The context switch itself (fiberSwitchAsm) and the initial stack frame
// layout are CPU-specific and live in the selected backend
// (vm/jit/<arch>/Fiber<Arch>.c) behind jit/TargetFiber.h.

// In-place growable stacks: a fiber reserves its whole stack region PROT_NONE
// (no RAM, no overcommit charge) and commits only a small window at the high end;
// the SIGSEGV handler grows the window downward on the guard fault. These are set
// once per OS thread by fiberInitStackGrowth (from schedulerInit).
static size_t gInitialCommit = 64 * 1024;
static long gPageSize = 4096;

void fiberInitStackGrowth(size_t initialCommitBytes)
{
	gPageSize = osPageSize();
	gInitialCommit = initialCommitBytes;
}

Fiber *fiberCreate(size_t stackSize)
{
	long pageSize = gPageSize;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
	// A sanitizer massively inflates C stack-frame sizes (shadow, redzones), and the
	// growth handler is disabled under a sanitizer, so a deep C call chain (e.g. a full
	// GC triggered from within JIT'd Smalltalk in a fiber) can overrun a normal stack.
	// Give every fiber a generous fully-committed stack under a sanitizer.
	if (stackSize < 64 * 1024 * 1024) {
		stackSize = 64 * 1024 * 1024;
	}
#endif

	// Reserve the whole region + a permanent floor page as PROT_NONE (address
	// space only), then commit a small window at the high end. The stack grows
	// DOWN; a fault in the reserved-but-uncommitted span triggers growth, and the
	// floor page below reserveFloor is the hard overflow backstop.
	stackSize = (stackSize + pageSize - 1) & ~((size_t) pageSize - 1);
	size_t mapSize = stackSize + pageSize;

	uint8_t *base = osPageReserve(mapSize);
	if (base == NULL) {
		return NULL;
	}

	size_t commit = (gInitialCommit + pageSize - 1) & ~((size_t) pageSize - 1);
	if (commit > stackSize) {
		commit = stackSize;
	}
	if (commit == 0) {
		commit = pageSize;
	}
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
	// A sanitizer intercepts SIGSEGV/sigaction/mprotect, which the SIGSEGV-driven stack
	// growth relies on (and the growth handler is a no-op under a sanitizer). Commit the
	// whole stack up front so no guard fault ever occurs during a sanitizer run.
	commit = stackSize;
#endif
	uint8_t *top = base + mapSize;
	uint8_t *committedLow = top - commit;
	if (!osPageCommit(committedLow, commit)) {
		osPageFree(base, mapSize);
		return NULL;
	}

	Fiber *fiber = calloc(1, sizeof(Fiber));
	fiber->stackBase = base;
	fiber->stackSize = mapSize;
	fiber->committedLow = committedLow;
	fiber->reserveFloor = base + pageSize; // never grow below this
	fiber->state = FIBER_SUSPENDED;
	fiber->waitFd = -1;
	fiber->tsanFiber = TSAN_CREATE_FIBER(); // NULL in a normal build
	// calloc zeroed entryBlock/process/cEntry/cArg/queueNext/dirty*.

	// Prime the stack so the first fiberSwitchAsm into it lands in
	// fiberTrampoline; the frame layout and alignment are the backend's business.
	fiber->sp = fiberTargetPrimeStack(top, fiberTrampoline);

	return fiber;
}


// Grow `fiber`'s committed RW window down to cover `faultAddr` (called from the
// SIGSEGV handler — must be async-signal-safe: only mprotect + plain arithmetic).
// Returns 1 if grown (retry the faulting instruction), 0 if the fault is outside
// the growable window (below the floor / not this fiber's stack → genuine fault).
int fiberGrowStack(Fiber *fiber, uintptr_t faultAddr)
{
	if (fiber == NULL || fiber->stackBase == NULL) {
		return 0;
	}
	uintptr_t floor = (uintptr_t) fiber->reserveFloor;
	uintptr_t clow = (uintptr_t) fiber->committedLow;
	if (faultAddr < floor || faultAddr >= clow) {
		return 0; // below the hard floor, or already committed / not in this window
	}
	uintptr_t pageMask = ~((uintptr_t) gPageSize - 1);
	uintptr_t faultPage = faultAddr & pageMask;
	uintptr_t chunkLow = clow - (64 * 1024); // grow at least a 64 KB chunk
	uintptr_t newLow = faultPage < chunkLow ? faultPage : chunkLow;
	if (newLow < floor) {
		newLow = floor;
	}
	if (!osPageCommit((void *) newLow, clow - newLow)) {
		return 0; // ENOMEM etc. → let the handler treat it as fatal
	}
	fiber->committedLow = (void *) newLow;
	return 1;
}


void fiberDestroy(Fiber *fiber)
{
	TSAN_DESTROY_FIBER(fiber->tsanFiber); // no-op in a normal build
	if (fiber->stackBase != NULL) {
		osPageFree(fiber->stackBase, fiber->stackSize);
	}
	free(fiber);
}


// Return the dead region of a just-parked fiber's stack to the OS. The stack
// grows DOWN with the guard page at the low (base) end, so live frames occupy
// [sp, top) and everything below the saved sp is unused / already-returned
// frames. MADV_DONTNEED drops those committed pages; they re-fault zero if the
// fiber later runs deeper (and the JIT prologue nil-inits every slot before
// reading it, so zero-fill is safe). Keep a one-page cushion below sp and never
// touch the guard page. Only worth a syscall when a real span can be reclaimed.
#define FIBER_STACK_RELEASE_THRESHOLD (32 * 1024)

void fiberReleaseIdleStack(Fiber *fiber)
{
	if (fiber->stackBase == NULL) {
		return; // the scheduler context has no fiber stack
	}
	long pageSize = gPageSize;
	// Only the committed (RW) window [committedLow, sp) is backed by pages; the
	// span below committedLow is PROT_NONE (reserved, unbacked). Reclaim the dead
	// part of the RW window below the saved sp. committedLow stays put (monotonic);
	// re-deepening re-faults these RW pages as ordinary zero-fill, not a SIGSEGV.
	uintptr_t lo = (uintptr_t) fiber->committedLow;
	uintptr_t hi = ((uintptr_t) fiber->sp - pageSize) & ~((uintptr_t) pageSize - 1); // cushion below sp
	if (hi > lo && (hi - lo) >= FIBER_STACK_RELEASE_THRESHOLD) {
		osPageRelease((void *) lo, hi - lo);
	}
}


void fiberTrampoline(void)
{
	// Runs on the new fiber's own stack the first time it is scheduled.
	schedulerFiberMain();
	// schedulerFiberMain never returns (it switches back to the scheduler
	// once the fiber's work is done).
	abort();
}
