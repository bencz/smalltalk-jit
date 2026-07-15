// ELFv1 ppc64 fiber switch: saves the full ELFv1 nonvolatile set — r14-r31,
// f14-f31 (ppc64 HAS callee-saved FP registers, unlike SysV x64), CR and LR
// — in the ELFV1_NV frame shape shared with the prime half (AbiElfV1.c) and
// the entry-stub hooks. No push/pop on POWER: one stdu creates the frame and
// writes the back chain; CR/LR go to the CALLER frame's ABI header slots,
// exactly like a compiled prologue. v20-v31/VSX are NOT saved — audit before
// enabling VMX/VSX in codegen (PORTING.md).
//
// This is the ONLY arch-only TU of the elfv1 binding: everything else is
// host-independent for native golden-testing; foreign hosts link a FAIL()
// stub for this symbol instead (vm/tests/EmitGoldenPpc64HostStub.c).
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "vm/jit/ppc64/ is BIG-ENDIAN ppc64 only (ppc64le has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/ppc64/abi/elfv1/FiberElfV1.h"

#define STR_(x) #x
#define STR(x) STR_(x)

// void fiberSwitchElfV1(void **saveSp, void *newSp)
//   r3 = saveSp (where to store the outgoing sp), r4 = newSp (incoming sp)
// ELFv1 requires hand-written asm to provide its own .opd function
// descriptor {entry, TOC, environ}; the global symbol names the descriptor,
// the local .L. label names the code.
__asm__(
	"	.section \".opd\",\"aw\"\n"
	"	.align 3\n"
	"	.globl fiberSwitchElfV1\n"
	"	.type fiberSwitchElfV1, @function\n"
	"fiberSwitchElfV1:\n"
	"	.quad .L.fiberSwitchElfV1, .TOC.@tocbase, 0\n"
	"	.previous\n"
	"	.text\n"
	"	.align 2\n"
	".L.fiberSwitchElfV1:\n"
	"	mflr  0\n"
	"	std   0, " STR(ELFV1_HEADER_LR_SAVE) "(1)\n"
	"	mfcr  0\n"
	"	stw   0, " STR(ELFV1_HEADER_CR_SAVE) "(1)\n"
	"	stdu  1, -" STR(ELFV1_NV_FRAME_SIZE) "(1)\n"
	"	std   2, " STR(ELFV1_NV_FRAME_TOC) "(1)\n"
	"	std   14, 48(1)\n"
	"	std   15, 56(1)\n"
	"	std   16, 64(1)\n"
	"	std   17, 72(1)\n"
	"	std   18, 80(1)\n"
	"	std   19, 88(1)\n"
	"	std   20, 96(1)\n"
	"	std   21, 104(1)\n"
	"	std   22, 112(1)\n"
	"	std   23, 120(1)\n"
	"	std   24, 128(1)\n"
	"	std   25, 136(1)\n"
	"	std   26, 144(1)\n"
	"	std   27, 152(1)\n"
	"	std   28, 160(1)\n"
	"	std   29, 168(1)\n"
	"	std   30, 176(1)\n"
	"	std   31, 184(1)\n"
	"	stfd  14, 192(1)\n"
	"	stfd  15, 200(1)\n"
	"	stfd  16, 208(1)\n"
	"	stfd  17, 216(1)\n"
	"	stfd  18, 224(1)\n"
	"	stfd  19, 232(1)\n"
	"	stfd  20, 240(1)\n"
	"	stfd  21, 248(1)\n"
	"	stfd  22, 256(1)\n"
	"	stfd  23, 264(1)\n"
	"	stfd  24, 272(1)\n"
	"	stfd  25, 280(1)\n"
	"	stfd  26, 288(1)\n"
	"	stfd  27, 296(1)\n"
	"	stfd  28, 304(1)\n"
	"	stfd  29, 312(1)\n"
	"	stfd  30, 320(1)\n"
	"	stfd  31, 328(1)\n"
	"	std   1, 0(3)\n"       // *saveSp = sp
	"	mr    1, 4\n"          // sp = newSp
	"	ld    2, " STR(ELFV1_NV_FRAME_TOC) "(1)\n"
	"	ld    14, 48(1)\n"
	"	ld    15, 56(1)\n"
	"	ld    16, 64(1)\n"
	"	ld    17, 72(1)\n"
	"	ld    18, 80(1)\n"
	"	ld    19, 88(1)\n"
	"	ld    20, 96(1)\n"
	"	ld    21, 104(1)\n"
	"	ld    22, 112(1)\n"
	"	ld    23, 120(1)\n"
	"	ld    24, 128(1)\n"
	"	ld    25, 136(1)\n"
	"	ld    26, 144(1)\n"
	"	ld    27, 152(1)\n"
	"	ld    28, 160(1)\n"
	"	ld    29, 168(1)\n"
	"	ld    30, 176(1)\n"
	"	ld    31, 184(1)\n"
	"	lfd   14, 192(1)\n"
	"	lfd   15, 200(1)\n"
	"	lfd   16, 208(1)\n"
	"	lfd   17, 216(1)\n"
	"	lfd   18, 224(1)\n"
	"	lfd   19, 232(1)\n"
	"	lfd   20, 240(1)\n"
	"	lfd   21, 248(1)\n"
	"	lfd   22, 256(1)\n"
	"	lfd   23, 264(1)\n"
	"	lfd   24, 272(1)\n"
	"	lfd   25, 280(1)\n"
	"	lfd   26, 288(1)\n"
	"	lfd   27, 296(1)\n"
	"	lfd   28, 304(1)\n"
	"	lfd   29, 312(1)\n"
	"	lfd   30, 320(1)\n"
	"	lfd   31, 328(1)\n"
	"	addi  1, 1, " STR(ELFV1_NV_FRAME_SIZE) "\n"
	"	ld    0, " STR(ELFV1_HEADER_LR_SAVE) "(1)\n"
	"	mtlr  0\n"
	"	lwz   0, " STR(ELFV1_HEADER_CR_SAVE) "(1)\n"
	"	mtcrf 0xff, 0\n"
	"	blr\n"
	"	.size fiberSwitchElfV1, .-.L.fiberSwitchElfV1\n"
	"	.section .note.GNU-stack,\"\",@progbits\n"
	"	.text\n"
);
