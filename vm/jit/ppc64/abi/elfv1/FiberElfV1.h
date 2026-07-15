#ifndef FIBER_ELFV1_H
#define FIBER_ELFV1_H

// ELFv1 stackful-coroutine pair (unique symbols; the TargetFiber.h names are
// bound in AbiElfV1Bind.c). The SWITCH is real POWER asm and exists only in
// ppc64 builds (FiberElfV1.c); foreign-host golden builds link a FAIL() stub
// (vm/tests/EmitGoldenPpc64HostStub.c) so the instance struct still links.
// The PRIME half is pure C and lives in AbiElfV1.c (host-independent, its
// slot layout is checked natively by the golden test).
void fiberSwitchElfV1(void **saveSp, void *newSp);
void *fiberPrimeStackElfV1(void *top, void (*entry)(void));

// The nonvolatile save frame both halves (and the entry-stub hooks) share.
// POWER has no push/pop: the frame is one stdu (which also writes the
// back-chain word at 0(r1)) and explicit std/stfd stores. CR and LR are
// saved in the CALLER frame's ABI header slots (+8/+16 above our frame),
// exactly as a compiled prologue would.
//
//   0(r1)   back chain (written by stdu)
//   8       (our callees' CR save slot — unused, we call nothing)
//   16      (our callees' LR save slot — unused)
//   24..39  compiler/linker dwords (ELFv1 header shape, unused)
//   40      TOC save (r2)
//   48      r14..r31   (18 * 8)
//   192     f14..f31   (18 * 8; f14-f31 are NONVOLATILE on ppc64 — unlike
//            SysV x64 there ARE callee-saved FP regs. v20-v31/VSX are NOT
//            saved: audit before enabling VMX/VSX codegen — PORTING.md)
//   336     total (16-byte aligned, ELFv1 requirement)
#define ELFV1_NV_FRAME_SIZE 336
#define ELFV1_NV_FRAME_TOC 40
#define ELFV1_NV_FRAME_GPRS 48
#define ELFV1_NV_FRAME_FPRS 192
#define ELFV1_HEADER_CR_SAVE 8
#define ELFV1_HEADER_LR_SAVE 16

#endif
