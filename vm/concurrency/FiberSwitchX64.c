// The x86-64 SysV context switch. One file per CPU, selected at link time.
//
// This is the whole machine-level surface of the fiber system: twenty
// instructions, and their content is dictated by the ABI rather than chosen.
// SysV says {rbp, rbx, r12-r15} survive a call, so those are exactly what a
// context switch must carry; everything else the compiler already treats as
// clobbered across the call to fiberSwitchAsm.
//
// Win64 additionally preserves rdi, rsi and xmm6-15, and must swap the TEB's
// stack-limit fields, so it is a different file rather than an #ifdef here.

#ifndef __x86_64__
#error "FiberSwitchX64.c is x86-64 only - check the ST_ARCH selection in CMakeLists.txt"
#endif

#include "concurrency/Fiber.h"
#include <stdint.h>

// void fiberSwitchAsm(void **saveSp, void *newSp)
//   rdi = where to store the outgoing rsp, rsi = the incoming rsp
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


// Lay out a fresh stack so the first switch into it pops six zeroed
// callee-saved slots and `ret`s straight into `entry`.
//
// The alignment contract is the fiddly part and getting it wrong shows up as a
// crash inside the first SSE instruction the entry function happens to use,
// which reads like anything but a stack bug. SysV requires rsp % 16 == 0 at the
// CALL, hence rsp % 16 == 8 on entry to the callee (the call pushed 8 bytes of
// return address). Here nothing calls: the `ret` in the switch pops the entry
// address and lands with rsp = base + 56. So base % 16 == 0 gives
// (base + 56) % 16 == 8, which is exactly the state a real call would leave.
void *fiberPrimeStack(void *top, void (*entry)(void))
{
	uintptr_t base = ((uintptr_t) top - 64) & ~(uintptr_t) 15;
	uintptr_t *slots = (uintptr_t *) base;
	slots[0] = 0; // r15
	slots[1] = 0; // r14
	slots[2] = 0; // r13
	slots[3] = 0; // r12
	slots[4] = 0; // rbx
	slots[5] = 0; // rbp
	slots[6] = (uintptr_t) entry; // what the switch's `ret` jumps to
	return (void *) base;
}
