#include "Fiber.h"
#include "Scheduler.h"
#include "Assert.h"
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

// void fiberSwitchAsm(void **saveSp, void *newSp)
//   rdi = saveSp (where to store the outgoing rsp), rsi = newSp (incoming rsp)
// Saves the callee-saved registers of the current context onto its own stack,
// stores rsp into *saveSp, loads rsp from newSp, restores that context's
// callee-saved registers and returns into it. Classic stackful coroutine swap.
__asm__(
	".text\n"
	".globl fiberSwitchAsm\n"
	".type fiberSwitchAsm, @function\n"
	"fiberSwitchAsm:\n"
	"	pushq %rbp\n"
	"	pushq %rbx\n"
	"	pushq %r12\n"
	"	pushq %r13\n"
	"	pushq %r14\n"
	"	pushq %r15\n"
	"	movq %rsp, (%rdi)\n"   // *saveSp = rsp
	"	movq %rsi, %rsp\n"     // rsp = newSp
	"	popq %r15\n"
	"	popq %r14\n"
	"	popq %r13\n"
	"	popq %r12\n"
	"	popq %rbx\n"
	"	popq %rbp\n"
	"	ret\n"
	".size fiberSwitchAsm, .-fiberSwitchAsm\n"
	".section .note.GNU-stack,\"\",@progbits\n"
	".text\n"
);


// In-place growable stacks: a fiber reserves its whole stack region PROT_NONE
// (no RAM, no overcommit charge) and commits only a small window at the high end;
// the SIGSEGV handler grows the window downward on the guard fault. These are set
// once per OS thread by fiberInitStackGrowth (from schedulerInit).
static size_t gInitialCommit = 64 * 1024;
static long gPageSize = 4096;

void fiberInitStackGrowth(size_t initialCommitBytes)
{
	long pg = sysconf(_SC_PAGESIZE);
	gPageSize = pg > 0 ? pg : 4096;
	gInitialCommit = initialCommitBytes;
}

Fiber *fiberCreate(size_t stackSize)
{
	long pageSize = gPageSize;

	// Reserve the whole region + a permanent floor page as PROT_NONE (address
	// space only), then commit a small window at the high end. The stack grows
	// DOWN; a fault in the reserved-but-uncommitted span triggers growth, and the
	// floor page below reserveFloor is the hard overflow backstop.
	stackSize = (stackSize + pageSize - 1) & ~((size_t) pageSize - 1);
	size_t mapSize = stackSize + pageSize;

	uint8_t *base = mmap(NULL, mapSize, PROT_NONE,
	                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (base == MAP_FAILED) {
		return NULL;
	}

	size_t commit = (gInitialCommit + pageSize - 1) & ~((size_t) pageSize - 1);
	if (commit > stackSize) {
		commit = stackSize;
	}
	if (commit == 0) {
		commit = pageSize;
	}
	uint8_t *top = base + mapSize;
	uint8_t *committedLow = top - commit;
	if (mprotect(committedLow, commit, PROT_READ | PROT_WRITE) != 0) {
		munmap(base, mapSize);
		return NULL;
	}

	Fiber *fiber = calloc(1, sizeof(Fiber));
	fiber->stackBase = base;
	fiber->stackSize = mapSize;
	fiber->committedLow = committedLow;
	fiber->reserveFloor = base + pageSize; // never grow below this
	fiber->state = FIBER_SUSPENDED;
	fiber->waitFd = -1;
	// calloc zeroed entryBlock/process/cEntry/cArg/queueNext/dirty*.

	// Prime the stack so the first fiberSwitchAsm into it pops six zeroed
	// callee-saved slots and `ret`s straight into fiberTrampoline, with the
	// ABI's 16-byte alignment (rsp % 16 == 8 at function entry). top-64 is inside
	// the committed window (commit >= one page).
	uintptr_t sp = ((uintptr_t) top - 64) & ~(uintptr_t) 15; // sp % 16 == 0
	uintptr_t *slots = (uintptr_t *) sp;
	slots[0] = 0; // r15
	slots[1] = 0; // r14
	slots[2] = 0; // r13
	slots[3] = 0; // r12
	slots[4] = 0; // rbx
	slots[5] = 0; // rbp
	slots[6] = (uintptr_t) fiberTrampoline; // ret target, at sp+48 (% 16 == 0)
	fiber->sp = (void *) sp;

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
	if (mprotect((void *) newLow, clow - newLow, PROT_READ | PROT_WRITE) != 0) {
		return 0; // ENOMEM etc. → let the handler treat it as fatal
	}
	fiber->committedLow = (void *) newLow;
	return 1;
}


void fiberDestroy(Fiber *fiber)
{
	if (fiber->stackBase != NULL) {
		munmap(fiber->stackBase, fiber->stackSize);
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
		madvise((void *) lo, hi - lo, MADV_DONTNEED);
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
