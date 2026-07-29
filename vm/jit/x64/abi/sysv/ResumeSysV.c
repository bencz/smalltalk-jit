// Entering compiled code at an arbitrary offset with a prebuilt register file.
//
// HERE, and not in vm/jit/, because it names registers. ADR 0009: no register
// name outside vm/jit/<arch>/, and this one is narrower still -- it names the
// SYSTEM V argument registers, so it belongs beside the ABI table that says
// which those are. Win64 passes its arguments in a different set and will need
// its own, exactly as it needs its own table.
//
// The neutral half -- reading the described values, re-boxing them, filling the
// register array -- is in vm/jit/DeoptResume.c and never learns what a register
// is.

#include "core/Object.h"

// System V argument order: rdi, rsi, rdx, rcx.
//
//   rdi  the address to jump to, inside a tier-1 method
//   rsi  the register file, registers[i] going to frame slot i
//   rdx  how many registers
//   rcx  frame bytes to reserve, a multiple of the stack alignment
//
// IT DOES RETURN, and by an indirect route worth stating: this function never
// executes a `ret` of its own. It jumps into tier-1 code, whose epilogue is
// `mov rsp, rbp; pop rbp; ret` -- which unwinds THIS frame, because this
// function set rbp up exactly the way tier 1's prologue would have, and returns
// to THIS function's caller with the answer in rax. From C it reads as an
// ordinary call answering a Value.
//
// THE FRAME LAW, which is the one both tiers share (jit/Lir.h): slot i lives at
// rbp - 8*(i+1). The copy loop below is that expression and nothing else.
//
// THE ALIGNMENT ARGUMENT, because getting it wrong surfaces inside the first SSE
// instruction some callee happens to use and reads like anything but a stack
// bug: at entry rsp is 8 mod 16 (the call pushed a return address), the push
// makes it 0, and reserving a multiple of 16 keeps it 0. That is precisely the
// state tier 1's own prologue leaves, which is the point -- the code being
// jumped into cannot tell it was entered from here.
__asm__(
	".text\n"
	".globl jitResumeAt\n"
	".type jitResumeAt,@function\n"
	"jitResumeAt:\n"
	"	pushq %rbp\n"
	"	movq %rsp, %rbp\n"
	"	subq %rcx, %rsp\n"
	"	xorq %rax, %rax\n"
	"1:\n"
	"	cmpq %rdx, %rax\n"
	"	jge 2f\n"
	"	movq (%rsi,%rax,8), %r8\n"
	"	movq %rax, %r9\n"
	"	shlq $3, %r9\n"
	"	movq %rbp, %r10\n"
	"	subq $8, %r10\n"
	"	subq %r9, %r10\n"
	"	movq %r8, (%r10)\n"
	"	incq %rax\n"
	"	jmp 1b\n"
	"2:\n"
	"	jmp *%rdi\n"
	".size jitResumeAt,.-jitResumeAt\n"
);
