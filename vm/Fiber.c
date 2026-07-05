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


Fiber *fiberCreate(size_t stackSize)
{
	long pageSize = sysconf(_SC_PAGESIZE);
	if (pageSize <= 0) {
		pageSize = 4096;
	}

	// round up to a page and add one guard page at the low (growth) end
	stackSize = (stackSize + pageSize - 1) & ~((size_t) pageSize - 1);
	size_t mapSize = stackSize + pageSize;

	uint8_t *base = mmap(NULL, mapSize, PROT_READ | PROT_WRITE,
	                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		return NULL;
	}
	// guard page: hitting it (stack overflow) faults instead of corrupting the heap
	mprotect(base, pageSize, PROT_NONE);

	Fiber *fiber = calloc(1, sizeof(Fiber));
	fiber->stackBase = base;
	fiber->stackSize = mapSize;
	fiber->state = FIBER_SUSPENDED;
	fiber->entryBlock = 0;
	fiber->process = 0;
	fiber->cEntry = NULL;
	fiber->cArg = NULL;
	fiber->queueNext = NULL;

	// Prime the stack so the first fiberSwitchAsm into it pops six zeroed
	// callee-saved slots and `ret`s straight into fiberTrampoline, with the
	// ABI's 16-byte alignment (rsp % 16 == 8 at function entry).
	uintptr_t top = (uintptr_t) base + mapSize;
	uintptr_t sp = (top - 64) & ~(uintptr_t) 15; // sp % 16 == 0
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


void fiberDestroy(Fiber *fiber)
{
	if (fiber->stackBase != NULL) {
		munmap(fiber->stackBase, fiber->stackSize);
	}
	free(fiber);
}


void fiberTrampoline(void)
{
	// Runs on the new fiber's own stack the first time it is scheduled.
	schedulerFiberMain();
	// schedulerFiberMain never returns (it switches back to the scheduler
	// once the fiber's work is done).
	abort();
}
