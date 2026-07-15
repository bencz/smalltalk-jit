#ifndef ASSEMBLER_PPC64_H
#define ASSEMBLER_PPC64_H

// ppc64 (BIG-ENDIAN, ELFv1) backend SKELETON: the minimal, REAL ISA model
// that arch-neutral code needs to compile (the Register type for
// jit/CodeGenerator.h signatures and the traits). ppc64le lives in its own
// backend directory. The instruction encoders do not exist yet — every code path
// that would need them FAIL()s in the stub TUs. Bring-up order (PORTING.md):
// implement the encoders here, golden-test them NATIVELY on the x86 build
// host (an EmitGoldenPpc64.c emits bytes into a buffer and compares — no
// target hardware needed), then run under qemu.
#include "jit/Assembler.h"
#include "jit/ppc64/TraitsPpc64.h"
#include "core/Assert.h"
#include <stddef.h>

// POWER general-purpose registers. Reservations fixed by the ABIs/kernel:
//   r0   sometimes reads as literal 0 in addressing forms — avoid as scratch
//   r1   stack pointer (back-chain ABI, no push/pop instructions)
//   r2   TOC pointer (ELFv1 always; ELFv2 across global calls)
//   r13  thread pointer (TLS, with the 0x7000 bias — see PORT_ME(tls))
// VM-internal role choices (PLACEHOLDERS until the real port begins; revisit
// against the register-allocator pool then):
//   TMP = r11 (a conventional intra-procedure scratch)
//   CTX = r31 (nonvolatile, mirrors x64's callee-saved R12 choice)
typedef enum {
	R0 = 0, R1 = 1, R2 = 2, R3 = 3, R4 = 4, R5 = 5, R6 = 6, R7 = 7,
	R8_PPC = 8, R9_PPC = 9, R10_PPC = 10, R11_PPC = 11, R12_PPC = 12,
	R13_PPC = 13, R14_PPC = 14, R15_PPC = 15, R16 = 16, R17 = 17,
	R18 = 18, R19 = 19, R20 = 20, R21 = 21, R22 = 22, R23 = 23,
	R24 = 24, R25 = 25, R26 = 26, R27 = 27, R28 = 28, R29 = 29,
	R30 = 30, R31 = 31,
	NO_REGISTER = -2,
} Register;

#define TMP R11_PPC
#define CTX R31

// TLS load delegate, same pattern as x64 (Abi.c -> gPpc64Abi->emitLoadTls).
// PORT_ME(tls): the ppc64 TLS ABI biases the thread pointer by 0x7000 —
// re-derive the tpoff contract before implementing.
void asmLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff);

#endif
