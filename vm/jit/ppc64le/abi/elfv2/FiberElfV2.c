// ELFv2 ppc64le fiber switch: saves the full ELFv2 nonvolatile set, r14-r31,
// f14-f31 (ppc64 HAS callee-saved FP registers, unlike SysV x64), CR and LR,
// in the ELFV2_NV frame shape shared with the prime half (AbiElfV2.c) and the
// entry-stub hooks. No push/pop on POWER: one stdu creates the frame and
// writes the back chain; CR/LR go to the CALLER frame's ABI header slots,
// exactly like a compiled prologue. v20-v31/VSX are NOT saved: unlike the BE
// port, whose baseline is pre-VSX power4, the ppc64le baseline IS power8, so
// audit this before trusting vectorized C across a switch (PORTING.md and
// vm/jit/ppc64le/DESIGN.md item 6).
//
// This is the ONLY arch-only TU of the elfv2 binding: everything else is
// host-independent for native golden-testing; foreign hosts link a FAIL()
// stub for this symbol instead (vm/tests/EmitGoldenPpc64leHostStub.c).
//
// Two deltas from FiberElfV1.c, both from ELFv2 having no function
// descriptors:
//   1. No `.opd` section and no `.L.` local label: the global symbol names the
//      CODE directly. No `.localentry` either, matching what gcc emits for a
//      function that never touches the TOC (verified in the ABI probe: its
//      TOC-free callee has no directive at all, so st_other stays 0 and the
//      global and local entry points coincide). `.abiversion 2` is already
//      emitted file-wide by the ppc64le compiler driving this TU.
//   2. `mr 12,0` on the restore path (see below).
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "vm/jit/ppc64le/ is LITTLE-ENDIAN ppc64le only (ppc64 has its own backend) - check ST_ARCH in CMakeLists.txt"
#endif

#include "jit/ppc64le/abi/elfv2/FiberElfV2.h"

#define STR_(x) #x
#define STR(x) STR_(x)

// void fiberSwitchElfV2(void **saveSp, void *newSp)
//   r3 = saveSp (where to store the outgoing sp), r4 = newSp (incoming sp)
//
// THE ELFv2 SUBTLETY, `mr 12,0` near the end: an ELFv2 function entered at its
// global entry derives its own TOC from r12 (`addis 2,12,.TOC.-.LCF@ha`), so a
// PRIMED fiber, whose LR slot holds the entry address itself, must arrive with
// r12 = entry or it computes a garbage r2 and dies on its first global access.
// Copying the LR value we are about to branch to into r12 is uniformly correct:
// for a primed fiber it IS the entry, and for a normal resume it is a return
// address inside the switch's C caller, where r12 is volatile and dead, so the
// write is inert. This is why the prime half does not seed the r2 slot.
__asm__(
	"	.text\n"
	"	.align 2\n"
	"	.globl fiberSwitchElfV2\n"
	"	.type fiberSwitchElfV2, @function\n"
	"fiberSwitchElfV2:\n"
	"	mflr  0\n"
	"	std   0, " STR(ELFV2_HEADER_LR_SAVE) "(1)\n"
	"	mfcr  0\n"
	"	stw   0, " STR(ELFV2_HEADER_CR_SAVE) "(1)\n"
	"	stdu  1, -" STR(ELFV2_NV_FRAME_SIZE) "(1)\n"
	"	std   2, " STR(ELFV2_NV_FRAME_TOC) "(1)\n"
	"	std   14, 32(1)\n"
	"	std   15, 40(1)\n"
	"	std   16, 48(1)\n"
	"	std   17, 56(1)\n"
	"	std   18, 64(1)\n"
	"	std   19, 72(1)\n"
	"	std   20, 80(1)\n"
	"	std   21, 88(1)\n"
	"	std   22, 96(1)\n"
	"	std   23, 104(1)\n"
	"	std   24, 112(1)\n"
	"	std   25, 120(1)\n"
	"	std   26, 128(1)\n"
	"	std   27, 136(1)\n"
	"	std   28, 144(1)\n"
	"	std   29, 152(1)\n"
	"	std   30, 160(1)\n"
	"	std   31, 168(1)\n"
	"	stfd  14, 176(1)\n"
	"	stfd  15, 184(1)\n"
	"	stfd  16, 192(1)\n"
	"	stfd  17, 200(1)\n"
	"	stfd  18, 208(1)\n"
	"	stfd  19, 216(1)\n"
	"	stfd  20, 224(1)\n"
	"	stfd  21, 232(1)\n"
	"	stfd  22, 240(1)\n"
	"	stfd  23, 248(1)\n"
	"	stfd  24, 256(1)\n"
	"	stfd  25, 264(1)\n"
	"	stfd  26, 272(1)\n"
	"	stfd  27, 280(1)\n"
	"	stfd  28, 288(1)\n"
	"	stfd  29, 296(1)\n"
	"	stfd  30, 304(1)\n"
	"	stfd  31, 312(1)\n"
	"	std   1, 0(3)\n"       // *saveSp = sp
	"	mr    1, 4\n"          // sp = newSp
	"	ld    2, " STR(ELFV2_NV_FRAME_TOC) "(1)\n"
	"	ld    14, 32(1)\n"
	"	ld    15, 40(1)\n"
	"	ld    16, 48(1)\n"
	"	ld    17, 56(1)\n"
	"	ld    18, 64(1)\n"
	"	ld    19, 72(1)\n"
	"	ld    20, 80(1)\n"
	"	ld    21, 88(1)\n"
	"	ld    22, 96(1)\n"
	"	ld    23, 104(1)\n"
	"	ld    24, 112(1)\n"
	"	ld    25, 120(1)\n"
	"	ld    26, 128(1)\n"
	"	ld    27, 136(1)\n"
	"	ld    28, 144(1)\n"
	"	ld    29, 152(1)\n"
	"	ld    30, 160(1)\n"
	"	ld    31, 168(1)\n"
	"	lfd   14, 176(1)\n"
	"	lfd   15, 184(1)\n"
	"	lfd   16, 192(1)\n"
	"	lfd   17, 200(1)\n"
	"	lfd   18, 208(1)\n"
	"	lfd   19, 216(1)\n"
	"	lfd   20, 224(1)\n"
	"	lfd   21, 232(1)\n"
	"	lfd   22, 240(1)\n"
	"	lfd   23, 248(1)\n"
	"	lfd   24, 256(1)\n"
	"	lfd   25, 264(1)\n"
	"	lfd   26, 272(1)\n"
	"	lfd   27, 280(1)\n"
	"	lfd   28, 288(1)\n"
	"	lfd   29, 296(1)\n"
	"	lfd   30, 304(1)\n"
	"	lfd   31, 312(1)\n"
	"	addi  1, 1, " STR(ELFV2_NV_FRAME_SIZE) "\n"
	"	ld    0, " STR(ELFV2_HEADER_LR_SAVE) "(1)\n"
	"	mtlr  0\n"
	"	mr    12, 0\n"         // ELFv2: r12 = the entry we are returning into
	"	lwz   0, " STR(ELFV2_HEADER_CR_SAVE) "(1)\n"
	"	mtcrf 0xff, 0\n"
	"	blr\n"
	"	.size fiberSwitchElfV2, .-fiberSwitchElfV2\n"
	"	.section .note.GNU-stack,\"\",@progbits\n"
	"	.text\n"
);
