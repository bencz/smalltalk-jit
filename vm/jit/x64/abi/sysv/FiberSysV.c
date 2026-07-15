// SysV x86-64 fiber switch: saves exactly the SysV nonvolatile GP set
// {rbp, rbx, r12-r15}. (Win64 additionally preserves rdi/rsi/xmm6-15 and must
// swap the TEB stack fields — a different file entirely; see PORTING.md.)
#ifndef __x86_64__
#error "vm/jit/x64/ is x86-64-only code - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "jit/x64/abi/sysv/FiberSysV.h"
#include <stdint.h>

// void fiberSwitchSysV(void **saveSp, void *newSp)
//   rdi = saveSp (where to store the outgoing rsp), rsi = newSp (incoming rsp)
// Saves the callee-saved registers of the current context onto its own stack,
// stores rsp into *saveSp, loads rsp from newSp, restores that context's
// callee-saved registers and returns into it. Classic stackful coroutine swap.
__asm__(
	".text\n"
	".globl fiberSwitchSysV\n"
	".type fiberSwitchSysV, @function\n"
	"fiberSwitchSysV:\n"
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
	".size fiberSwitchSysV, .-fiberSwitchSysV\n"
	".section .note.GNU-stack,\"\",@progbits\n"
	".text\n"
);

// Prime the stack so the first fiberSwitchSysV into it pops the six zeroed
// callee-saved slots above and `ret`s straight into `entry`, with the SysV
// alignment contract (rsp % 16 == 8 at function entry). top-64 stays inside
// the committed window (the caller commits at least one page).
void *fiberPrimeStackSysV(void *top, void (*entry)(void))
{
	uintptr_t sp = ((uintptr_t) top - 64) & ~(uintptr_t) 15; // sp % 16 == 0
	uintptr_t *slots = (uintptr_t *) sp;
	slots[0] = 0; // r15
	slots[1] = 0; // r14
	slots[2] = 0; // r13
	slots[3] = 0; // r12
	slots[4] = 0; // rbx
	slots[5] = 0; // rbp
	slots[6] = (uintptr_t) entry; // ret target, at sp+48 (% 16 == 0)
	return (void *) sp;
}
