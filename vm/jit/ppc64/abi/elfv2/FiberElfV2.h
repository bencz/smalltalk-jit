#ifndef FIBER_ELFV2_H
#define FIBER_ELFV2_H

// ELFv2 stackful-coroutine pair (unique symbols; the TargetFiber.h names are
// bound in AbiElfV2Bind.c). The SWITCH is real POWER asm and exists only in
// ppc64le builds (FiberElfV2.c); foreign-host golden builds link a FAIL() stub
// (vm/tests/EmitGoldenPpc64leHostStub.c) so the instance struct still links.
// The PRIME half is pure C and lives in AbiElfV2.c (host-independent, its
// slot layout is checked natively by the golden test).
void fiberSwitchElfV2(void **saveSp, void *newSp);
void *fiberPrimeStackElfV2(void *top, void (*entry)(void));

// The nonvolatile save frame both halves (and the entry-stub hooks) share.
// POWER has no push/pop: the frame is one stdu (which also writes the
// back-chain word at 0(r1)) and explicit std/stfd stores. CR and LR are
// saved in the CALLER frame's ABI header slots (+8/+16 above our frame),
// exactly as a compiled prologue would.
//
// The ELFv2 header is 32 bytes, against ELFv1's 48: it drops the two
// compiler/linker dwords at 24..39 and moves the TOC save from 40 to 24. Every
// constant below therefore shifts by 16 relative to FiberElfV1.h.
//
//   0(r1)   back chain (written by stdu)
//   8       (our callees' CR save slot; unused, we call nothing)
//   16      (our callees' LR save slot; unused)
//   24      TOC save (r2)
//   32      r14..r31   (18 * 8)
//   176     f14..f31   (18 * 8; f14-f31 are NONVOLATILE on ppc64, unlike
//            SysV x64 there ARE callee-saved FP regs. v20-v31/VSX are NOT
//            saved, and unlike the BE port that gap is not theoretical here:
//            the ppc64le baseline is POWER8, so the compiler CAN allocate a
//            nonvolatile vector register across a switch. See PORTING.md and
//            vm/jit/ppc64/DESIGN-elfv2.md item 6)
//   320     total (16-byte aligned, ELFv2 requirement)
#define ELFV2_NV_FRAME_SIZE 320
#define ELFV2_NV_FRAME_TOC 24
#define ELFV2_NV_FRAME_GPRS 32
#define ELFV2_NV_FRAME_FPRS 176
#define ELFV2_HEADER_CR_SAVE 8
#define ELFV2_HEADER_LR_SAVE 16

#endif
