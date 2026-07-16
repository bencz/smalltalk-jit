#ifndef ASSEMBLER_PPC64LE_H
#define ASSEMBLER_PPC64LE_H

// ppc64le (LITTLE-ENDIAN, ELFv2) backend: the POWER ISA model, Register enum,
// register-allocation pool, and the instruction ENCODERS. Big-endian ppc64
// lives in its own backend directory (the ecosystem treats them as distinct
// arches and the codegen diverges beyond the ABI); this file is a deliberate
// COPY of vm/jit/ppc64/AssemblerPpc64.h in which exactly SIX functions differ:
// ppcLoadWord/ppcStoreWord, asmLi64Read/asmLi64Patch (byte order), and
// asmPpcEmitWord/asmPpcLabelBind (correct for free, they funnel through the
// first two). The other 95 encoders build a 32-bit word out of bit-fields and
// are byte-order-neutral by construction. Deltas: vm/jit/ppc64le/DESIGN.md.
//
// HOST-INDEPENDENT BY DESIGN: every encoder builds a 32-bit instruction word
// and stores it explicitly LITTLE-ENDIAN, which IS native byte order on every
// host this backend ever executes on, and lets any build host (the x86 dev
// box) compile these emitters, run them into an AssemblerBuffer and
// byte-compare the output against pinned vectors validated with a cross
// objdump (vm/tests/EmitGoldenPpc64le.c, ST_PPC64LE_EMIT_TEST), bring-up rung
// 1 of PORTING.md, no target hardware involved. Do NOT "simplify" the explicit
// shifts into native-order stores: that would silently emit x86-order words
// when the golden runs on the dev host.
//
// Encoder operand order mirrors the ASSEMBLY MNEMONIC (destination first):
// asmAddi(b, rt, ra, si) == addi rt, ra, si; loads/stores read like
// ld rt, disp(ra) == asmLd(b, rt, disp, ra).
#ifdef ASSEMBLER_PPC64_H
#error "AssemblerPpc64.h (BE) and AssemblerPpc64le.h (LE) are SEPARATE backends: each fixes the instruction word's byte order for the whole translation unit, so no TU may include both."
#endif

#include "jit/Assembler.h"
#include "jit/ppc64le/TraitsPpc64le.h"
#include "core/Assert.h"
#include <stddef.h>

// POWER general-purpose registers. Reservations fixed by the ABIs/kernel:
//   r0   sometimes reads as literal 0 in addressing forms, usable only as a
//        computation target / X-form operand, NEVER as a D/DS-form base
//   r1   stack pointer (back-chain ABI, no push/pop instructions)
//   r2   TOC pointer (ELFv2: saved at 24(r1) across indirect/global calls)
//   r13  thread pointer (TLS; tpoffs are computed at runtime from r13, so the
//        ABI's 0x7000 bias cancels out of the arithmetic, see Thread.c)
// VM-internal roles, IDENTICAL to the ppc64 backend (vm/jit/ppc64/DESIGN.md
// pins the x64 mapping; do not re-derive them here):
//   FP   = r31  frame pointer, StackFrame*-compatible   (x64 RBP)
//   CTX  = r30  context                                 (x64 R12)
//   TGT  = r12  native-code entry across sends          (x64 R11)
//   TMP  = r11  per-instruction scratch                 (x64 R10)
//   TMP2 = r10  second scratch, owned by the tagged-base
//               ld/std helpers below                    (no x64 analog)
//   r3   result (x64 RAX) and dispatch scratch A (x64 RDI: class / C arg0)
//   r4   dispatch scratch B (x64 RSI: selector / C arg1)
//   r5   dispatch scratch C (x64 RDX: size, hash / C arg2)
//   r6/r7/r8 extra scratch (x64 RCX/R8/R9)
//   r14  callee-saved scratch inside entry-hook frames  (x64 RBX)
//   r15  prim scratch preserved across CCalls           (x64 R13)
// WARNING: TGT = r12 is a HARMONY with ELFv2, not a collision. The ABI requires
// r12 to hold the callee's global-entry address on an indirect call, which is
// exactly what our send-target register already carries. The consequence is
// that ELFv2's emitCallCFunction clobbers TGT (the ELFv1 one clobbers TMP), so
// every C-call site must have TGT dead. See vm/jit/ppc64le/DESIGN.md item 3.
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
#define TMP2 R10_PPC
#define CTX R30
#define FP R31
#define TGT R12_PPC

// Allocation pool: nonvolatile-heavy so values survive C calls without
// spills, plus one volatile. Excludes every reserved/role register above.
// The ABI golden test enforces the cross-member invariant (spill list ==
// C-clobbered subset of pool ∪ scratch extras), update AbiElfV2's
// callerSavedSpill and the golden's extras together with this pool.
static uint8_t Ppc64Registers[] = {
	R16, R17, R18, R19, R20, R21, R22, R23, R24,
	R9_PPC,
};
static AvailableRegs Ppc64AvailableRegs = {
	.regsSize = sizeof(Ppc64Registers),
	.regs = Ppc64Registers,
};

// Condition-register fields/bits and branch BO operands (Power ISA B-form).
enum {
	CR_LT = 0,
	CR_GT = 1,
	CR_EQ = 2,
	CR_SO = 3,
};

enum {
	BO_IF_FALSE = 4,   // branch if CR bit clear
	BO_IF_TRUE = 12,   // branch if CR bit set
	BO_ALWAYS = 20,
};

enum {
	SPR_LR = 8,
	SPR_CTR = 9,
};

// ---- raw word emission (explicitly little-endian) ---------------------------
// WARNING: DELTA vs ppc64 (BE), functions 1 and 2. Explicit shifts, NOT a
// native-order store: this header is compiled on the x86 dev host for the
// golden, where native order is the WRONG one.

static inline uint32_t ppcLoadWord(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
		| ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static inline void ppcStoreWord(uint8_t *p, uint32_t word)
{
	p[0] = (uint8_t) word;
	p[1] = (uint8_t) (word >> 8);
	p[2] = (uint8_t) (word >> 16);
	p[3] = (uint8_t) (word >> 24);
}

static inline void asmPpcEmitWord(AssemblerBuffer *buffer, uint32_t word)
{
	asmEnsureCapacity(buffer);
	ppcStoreWord(buffer->p, word);
	buffer->p += sizeof(uint32_t);
}

// ---- fixed-point loads/stores ------------------------------------------------

// DS-form (64-bit): displacement must be a multiple of 4 in [-32768, 32764].
static inline uint32_t ppcDsForm(uint8_t opcd, Register rt, ptrdiff_t disp, Register ra, uint8_t xo)
{
	// A negative register here is a SPILLED_REG/NO_REGISTER leak: x64 masks
	// it into a usable encoding by accident, POWER must fail loudly.
	ASSERT((int) rt >= 0 && (int) ra >= 0);
	ASSERT((disp & 3) == 0 && -32768 <= disp && disp <= 32764);
	return ((uint32_t) opcd << 26) | ((uint32_t) (rt & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) disp & 0xFFFC) | xo;
}

static inline void asmLd(AssemblerBuffer *buffer, Register rt, ptrdiff_t disp, Register ra)
{
	asmPpcEmitWord(buffer, ppcDsForm(58, rt, disp, ra, 0));
}

static inline void asmStd(AssemblerBuffer *buffer, Register rs, ptrdiff_t disp, Register ra)
{
	asmPpcEmitWord(buffer, ppcDsForm(62, rs, disp, ra, 0));
}

// std with update: writes the back-chain word and lowers r1 in one
// instruction, THE ppc64 frame push (there is no push/pop on POWER).
static inline void asmStdu(AssemblerBuffer *buffer, Register rs, ptrdiff_t disp, Register ra)
{
	asmPpcEmitWord(buffer, ppcDsForm(62, rs, disp, ra, 1));
}

// D-form (32-bit and floating-point): 16-bit signed displacement.
static inline uint32_t ppcDForm(uint8_t opcd, uint8_t rt, uint8_t ra, ptrdiff_t d)
{
	ASSERT(rt <= 31 && ra <= 31); // a 255 here is a (uint8_t) SPILLED_REG leak
	ASSERT(-32768 <= d && d <= 32767);
	return ((uint32_t) opcd << 26) | ((uint32_t) (rt & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) d & 0xFFFF);
}

static inline void asmLwz(AssemblerBuffer *buffer, Register rt, ptrdiff_t disp, Register ra)
{
	asmPpcEmitWord(buffer, ppcDForm(32, (uint8_t) rt, (uint8_t) ra, disp));
}

static inline void asmStw(AssemblerBuffer *buffer, Register rs, ptrdiff_t disp, Register ra)
{
	asmPpcEmitWord(buffer, ppcDForm(36, (uint8_t) rs, (uint8_t) ra, disp));
}

// Floating-point doubleword load/store (frt is a raw FPR number 0..31; the
// fiber switch and entry stub must preserve the NONVOLATILE f14-f31, unlike
// SysV x64, ppc64 has callee-saved FP registers).
static inline void asmLfd(AssemblerBuffer *buffer, int frt, ptrdiff_t disp, Register ra)
{
	ASSERT(0 <= frt && frt <= 31);
	asmPpcEmitWord(buffer, ppcDForm(50, (uint8_t) frt, (uint8_t) ra, disp));
}

static inline void asmStfd(AssemblerBuffer *buffer, int frs, ptrdiff_t disp, Register ra)
{
	ASSERT(0 <= frs && frs <= 31);
	asmPpcEmitWord(buffer, ppcDForm(54, (uint8_t) frs, (uint8_t) ra, disp));
}

// ---- arithmetic / logical / moves --------------------------------------------

// addi/addis read RA=0 as the LITERAL ZERO, not r0, that form is the li/lis
// alias below; the plain emitters assert it away to catch accidents.
static inline void asmAddi(AssemblerBuffer *buffer, Register rt, Register ra, ptrdiff_t si)
{
	ASSERT(ra != R0);
	asmPpcEmitWord(buffer, ppcDForm(14, (uint8_t) rt, (uint8_t) ra, si));
}

static inline void asmAddis(AssemblerBuffer *buffer, Register rt, Register ra, ptrdiff_t si)
{
	ASSERT(ra != R0);
	asmPpcEmitWord(buffer, ppcDForm(15, (uint8_t) rt, (uint8_t) ra, si));
}

static inline void asmLi(AssemblerBuffer *buffer, Register rt, ptrdiff_t si)
{
	asmPpcEmitWord(buffer, ppcDForm(14, (uint8_t) rt, 0, si));
}

static inline void asmLis(AssemblerBuffer *buffer, Register rt, ptrdiff_t si)
{
	asmPpcEmitWord(buffer, ppcDForm(15, (uint8_t) rt, 0, si));
}

// ori/oris take an UNSIGNED 16-bit immediate (no sign extension). Note the
// ISA operand order: the source register sits in the RT slot.
static inline uint32_t ppcOriForm(uint8_t opcd, Register ra, Register rs, uint32_t ui)
{
	ASSERT(ui <= 0xFFFF);
	return ((uint32_t) opcd << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ui;
}

static inline void asmOri(AssemblerBuffer *buffer, Register ra, Register rs, uint32_t ui)
{
	asmPpcEmitWord(buffer, ppcOriForm(24, ra, rs, ui));
}

static inline void asmOris(AssemblerBuffer *buffer, Register ra, Register rs, uint32_t ui)
{
	asmPpcEmitWord(buffer, ppcOriForm(25, ra, rs, ui));
}

static inline void asmNop(AssemblerBuffer *buffer)
{
	asmPpcEmitWord(buffer, 0x60000000); // ori r0, r0, 0
}

static inline void asmOr(AssemblerBuffer *buffer, Register ra, Register rs, Register rb)
{
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (rb & 31) << 11) | (444u << 1));
}

static inline void asmMr(AssemblerBuffer *buffer, Register ra, Register rs)
{
	asmOr(buffer, ra, rs, rs);
}

// MD-form rotate; sldi(ra, rs, n) == rldicr(ra, rs, n, 63-n). The 6-bit
// mask field is stored rotated (me[1:5] || me[0]) and the 6-bit shift is
// split (sh[0:4] in bits 16-20, sh[5] in bit 30), Power ISA MD-form.
static inline void asmRldicr(AssemblerBuffer *buffer, Register ra, Register rs, int sh, int me)
{
	ASSERT(0 <= sh && sh <= 63 && 0 <= me && me <= 63);
	asmPpcEmitWord(buffer, (30u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (sh & 31) << 11)
		| (((((uint32_t) me & 31) << 1) | ((uint32_t) me >> 5)) << 5)
		| (1u << 2) | (((uint32_t) sh >> 5) << 1));
}

static inline void asmSldi(AssemblerBuffer *buffer, Register ra, Register rs, int n)
{
	asmRldicr(buffer, ra, rs, n, 63 - n);
}

// cmpdi crf, ra, si, 64-bit signed compare with immediate (L bit set).
static inline void asmCmpdi(AssemblerBuffer *buffer, int crf, Register ra, ptrdiff_t si)
{
	ASSERT(0 <= crf && crf <= 7);
	ASSERT(-32768 <= si && si <= 32767);
	asmPpcEmitWord(buffer, (11u << 26) | ((uint32_t) crf << 23) | (1u << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) si & 0xFFFF));
}

// ---- 64-bit immediates ---------------------------------------------------------

// Materialize an arbitrary 64-bit immediate in a FIXED 5-instruction shape
// (lis; ori; sldi 32; oris; ori), the ppc64 analog of x64's movabs: constant
// size, patchable in place (asmLi64Patch), works for every value. The sign
// junk lis/ori leave in the high word is shifted out by the sldi.
static inline void asmLi64(AssemblerBuffer *buffer, Register rt, uint64_t imm)
{
	asmLis(buffer, rt, (int16_t) (imm >> 48));
	asmOri(buffer, rt, rt, (uint32_t) ((imm >> 32) & 0xFFFF));
	asmSldi(buffer, rt, rt, 32);
	asmOris(buffer, rt, rt, (uint32_t) ((imm >> 16) & 0xFFFF));
	asmOri(buffer, rt, rt, (uint32_t) (imm & 0xFFFF));
}

// Reassemble the 64-bit immediate of an emitted asmLi64 sequence, the read
// half of the targetReadCodePointer/targetWriteCodePointer contract
// (jit/TargetCodePatch.h) that the GC's baked-pointer walks use.
//
// WARNING: DELTA vs ppc64 (BE), function 4. THE most dangerous hand-translation
// of this port (silent wrong code, and the GC patches live pointers). For the
// word of instruction k at byte offset 4k, BE stores [W>>24, W>>16, W>>8, W]
// and LE stores [W, W>>8, W>>16, W>>24], so relative to the BE version:
//   opcode check byte  seq[4k]   -> seq[4k+3]
//   imm16 HIGH byte    seq[4k+2] -> seq[4k+1]
//   imm16 LOW byte     seq[4k+3] -> seq[4k]
// Pinned by the golden's checkLi64Patch() and the binutils oracle.
static inline uint64_t asmLi64Read(const uint8_t *seq)
{
	ASSERT(seq[3] >> 2 == 15 && seq[7] >> 2 == 24 && seq[11] >> 2 == 30
		&& seq[15] >> 2 == 25 && seq[19] >> 2 == 24);
	return ((uint64_t) seq[1] << 56) | ((uint64_t) seq[0] << 48)
		| ((uint64_t) seq[5] << 40) | ((uint64_t) seq[4] << 32)
		| ((uint64_t) seq[13] << 24) | ((uint64_t) seq[12] << 16)
		| ((uint64_t) seq[17] << 8) | (uint64_t) seq[16];
}

// Rewrite the four 16-bit immediate halves of an emitted asmLi64 sequence
// (the sldi word is immutable). This is what a GC pointer-patch loop and
// absolute-address fixups will use; `seq` points at the first byte of the
// 5-word sequence in FINAL (or buffer) memory.
static inline void asmLi64Patch(uint8_t *seq, uint64_t imm)
{
	// Defensive: primary opcodes of lis/ori/rldicr/oris/ori live in the top
	// 6 bits of each word == top 6 bits of its LAST byte in little-endian
	// memory (BE reads them off the first byte instead).
	ASSERT(seq[3] >> 2 == 15 && seq[7] >> 2 == 24 && seq[11] >> 2 == 30
		&& seq[15] >> 2 == 25 && seq[19] >> 2 == 24);
	seq[1] = (uint8_t) (imm >> 56); seq[0] = (uint8_t) (imm >> 48);
	seq[5] = (uint8_t) (imm >> 40); seq[4] = (uint8_t) (imm >> 32);
	seq[13] = (uint8_t) (imm >> 24); seq[12] = (uint8_t) (imm >> 16);
	seq[17] = (uint8_t) (imm >> 8); seq[16] = (uint8_t) imm;
}

// ---- special-purpose registers -------------------------------------------------

// The 10-bit SPR number is encoded with its halves swapped (spr[5:9]||spr[0:4]).
static inline uint32_t ppcSprField(int spr)
{
	ASSERT(0 <= spr && spr <= 1023);
	return ((((uint32_t) spr & 31) << 5) | ((uint32_t) spr >> 5)) << 11;
}

static inline void asmMtspr(AssemblerBuffer *buffer, int spr, Register rs)
{
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rs & 31) << 21)
		| ppcSprField(spr) | (467u << 1));
}

static inline void asmMfspr(AssemblerBuffer *buffer, Register rt, int spr)
{
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rt & 31) << 21)
		| ppcSprField(spr) | (339u << 1));
}

static inline void asmMtlr(AssemblerBuffer *buffer, Register rs)
{
	asmMtspr(buffer, SPR_LR, rs);
}

static inline void asmMflr(AssemblerBuffer *buffer, Register rt)
{
	asmMfspr(buffer, rt, SPR_LR);
}

static inline void asmMtctr(AssemblerBuffer *buffer, Register rs)
{
	asmMtspr(buffer, SPR_CTR, rs);
}

static inline void asmMfcr(AssemblerBuffer *buffer, Register rt)
{
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rt & 31) << 21) | (19u << 1));
}

static inline void asmMtcrf(AssemblerBuffer *buffer, uint32_t fxm, Register rs)
{
	ASSERT(fxm <= 0xFF);
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rs & 31) << 21)
		| (fxm << 12) | (144u << 1));
}

// ---- branches ------------------------------------------------------------------

static inline void asmBlr(AssemblerBuffer *buffer)
{
	asmPpcEmitWord(buffer, 0x4E800020);
}

static inline void asmBctr(AssemblerBuffer *buffer)
{
	asmPpcEmitWord(buffer, 0x4E800420);
}

static inline void asmBctrl(AssemblerBuffer *buffer)
{
	asmPpcEmitWord(buffer, 0x4E800421);
}

// POWER branch displacements are relative to the branch instruction's OWN
// address (not the next instruction, as on x86) and are embedded in the low
// bits of the instruction word, so labels get ppc64-specific emit/bind
// logic: forward references write a zero displacement and asmPpcLabelBind
// patches the WORD once the target offset is known, telling the I-form
// (b, ±32MB) from the B-form (bc, ±32KB) by the primary opcode. Same contract
// as x64 labels: at most ONE forward reference per label.
// WARNING: NEVER route these through the generic asmLabelBind
// (jit/Assembler.h): it is x86-shaped (displacement relative to the END) and
// label->size == 4 makes its `case 4` look applicable, wrong code, no
// diagnostic.

static inline void ppcCheckBranchDisp26(ptrdiff_t disp)
{
	ASSERT((disp & 3) == 0 && -0x2000000 <= disp && disp <= 0x1FFFFFC);
	(void) disp;
}

static inline void ppcCheckBranchDisp16(ptrdiff_t disp)
{
	ASSERT((disp & 3) == 0 && -0x8000 <= disp && disp <= 0x7FFC);
	(void) disp;
}

static inline void asmB(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	if (label->isBound) {
		ptrdiff_t disp = label->offset - asmOffset(buffer);
		ppcCheckBranchDisp26(disp);
		asmPpcEmitWord(buffer, (18u << 26) | ((uint32_t) disp & 0x03FFFFFC));
	} else {
		ASSERT(!label->isResolved);
		label->isResolved = 1;
		label->offset = asmOffset(buffer);
		label->size = sizeof(uint32_t);
		asmPpcEmitWord(buffer, 18u << 26);
	}
}

static inline void asmBc(AssemblerBuffer *buffer, int bo, int bi, AssemblerLabel *label)
{
	uint32_t word = (16u << 26) | ((uint32_t) (bo & 31) << 21) | ((uint32_t) (bi & 31) << 16);
	if (label->isBound) {
		ptrdiff_t disp = label->offset - asmOffset(buffer);
		ppcCheckBranchDisp16(disp);
		asmPpcEmitWord(buffer, word | ((uint32_t) disp & 0xFFFC));
	} else {
		ASSERT(!label->isResolved);
		label->isResolved = 1;
		label->offset = asmOffset(buffer);
		label->size = sizeof(uint32_t);
		asmPpcEmitWord(buffer, word);
	}
}

static inline void asmPpcLabelBind(AssemblerBuffer *buffer, AssemblerLabel *label, ptrdiff_t offset)
{
	ASSERT(!label->isBound);
	label->isBound = 1;

	if (label->isResolved) {
		uint8_t *p = buffer->buffer + label->offset;
		uint32_t word = ppcLoadWord(p);
		ptrdiff_t disp = offset - label->offset;
		switch (word >> 26) {
		case 18:
			ppcCheckBranchDisp26(disp);
			word |= (uint32_t) disp & 0x03FFFFFC;
			break;
		case 16:
			ppcCheckBranchDisp16(disp);
			word |= (uint32_t) disp & 0xFFFC;
			break;
		default:
			FAIL();
		}
		ppcStoreWord(p, word);
	}
	label->offset = offset;
}

// CR0-based condition aliases (BI = 4*crf + bit).
static inline void asmBeq(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	asmBc(buffer, BO_IF_TRUE, CR_EQ, label);
}

static inline void asmBne(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	asmBc(buffer, BO_IF_FALSE, CR_EQ, label);
}

static inline void asmBlt(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	asmBc(buffer, BO_IF_TRUE, CR_LT, label);
}

static inline void asmBge(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	asmBc(buffer, BO_IF_FALSE, CR_LT, label);
}

static inline void asmBgt(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	asmBc(buffer, BO_IF_TRUE, CR_GT, label);
}

static inline void asmBle(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	asmBc(buffer, BO_IF_FALSE, CR_GT, label);
}

// ---- batch 2: the code-generator instruction set -----------------------------
// Everything the CodeGenerator/StubCode/Primitives translation needs beyond
// the bring-up batch. Same rules: mnemonic operand order, explicit LE words,
// oracle-validated.

// D-form sub-word loads/stores (zero-extending loads). Used for every
// sub-word C struct field at its offsetof(), natural-width accesses are
// endian-correct by construction (DESIGN.md, endianness rules).
static inline void asmLbz(AssemblerBuffer *buffer, Register rt, ptrdiff_t disp, Register ra)
{
	asmPpcEmitWord(buffer, ppcDForm(34, (uint8_t) rt, (uint8_t) ra, disp));
}

static inline void asmStb(AssemblerBuffer *buffer, Register rs, ptrdiff_t disp, Register ra)
{
	asmPpcEmitWord(buffer, ppcDForm(38, (uint8_t) rs, (uint8_t) ra, disp));
}

static inline void asmLhz(AssemblerBuffer *buffer, Register rt, ptrdiff_t disp, Register ra)
{
	asmPpcEmitWord(buffer, ppcDForm(40, (uint8_t) rt, (uint8_t) ra, disp));
}

static inline void asmSth(AssemblerBuffer *buffer, Register rs, ptrdiff_t disp, Register ra)
{
	asmPpcEmitWord(buffer, ppcDForm(44, (uint8_t) rs, (uint8_t) ra, disp));
}

// X-form indexed loads/stores (EA = RA|0 + RB), the substitute for x64's
// scaled-index addressing (scale with asmSldi into a scratch first).
static inline uint32_t ppcXForm(Register rt, Register ra, Register rb, uint32_t xo)
{
	ASSERT((int) rt >= 0 && (int) ra >= 0 && (int) rb >= 0);
	return (31u << 26) | ((uint32_t) (rt & 31) << 21) | ((uint32_t) (ra & 31) << 16)
		| ((uint32_t) (rb & 31) << 11) | (xo << 1);
}

static inline void asmLdx(AssemblerBuffer *buffer, Register rt, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXForm(rt, ra, rb, 21));
}

static inline void asmStdx(AssemblerBuffer *buffer, Register rs, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXForm(rs, ra, rb, 149));
}

static inline void asmLbzx(AssemblerBuffer *buffer, Register rt, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXForm(rt, ra, rb, 87));
}

static inline void asmStbx(AssemblerBuffer *buffer, Register rs, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXForm(rs, ra, rb, 215));
}

// ---- compares ------------------------------------------------------------------

// cmpd/cmpld: 64-bit register-register compare, signed/unsigned (x64 cmp +
// signed/unsigned condition split maps to WHICH compare is emitted here).
static inline void asmCmpd(AssemblerBuffer *buffer, int crf, Register ra, Register rb)
{
	ASSERT(0 <= crf && crf <= 7);
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) crf << 23) | (1u << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (rb & 31) << 11));
}

static inline void asmCmpld(AssemblerBuffer *buffer, int crf, Register ra, Register rb)
{
	ASSERT(0 <= crf && crf <= 7);
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) crf << 23) | (1u << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (rb & 31) << 11) | (32u << 1));
}

static inline void asmCmpldi(AssemblerBuffer *buffer, int crf, Register ra, uint32_t ui)
{
	ASSERT(0 <= crf && crf <= 7);
	ASSERT(ui <= 0xFFFF);
	asmPpcEmitWord(buffer, (10u << 26) | ((uint32_t) crf << 23) | (1u << 21)
		| ((uint32_t) (ra & 31) << 16) | ui);
}

// ---- XO-form arithmetic ----------------------------------------------------------
// OE=1/Rc=1 variants exist for the tagged-integer overflow protocol: `addo.`
// then `bso` on CR0.SO. XER[SO] is STICKY, fast paths do not clear it; a
// stale SO only causes a false-positive fall-through to the send/dispatch
// path, which re-arms with asmClearXerSo (DESIGN.md, arithmetic rules).

static inline uint32_t ppcXoForm(Register rt, Register ra, Register rb, uint32_t xo, _Bool oe, _Bool rc)
{
	return (31u << 26) | ((uint32_t) (rt & 31) << 21) | ((uint32_t) (ra & 31) << 16)
		| ((uint32_t) (rb & 31) << 11) | ((uint32_t) oe << 10) | (xo << 1) | (uint32_t) rc;
}

static inline void asmAdd(AssemblerBuffer *buffer, Register rt, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXoForm(rt, ra, rb, 266, 0, 0));
}

static inline void asmAddoDot(AssemblerBuffer *buffer, Register rt, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXoForm(rt, ra, rb, 266, 1, 1));
}

// subf rt, ra, rb computes rb - ra (note the operand roles!)
static inline void asmSubf(AssemblerBuffer *buffer, Register rt, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXoForm(rt, ra, rb, 40, 0, 0));
}

static inline void asmSubfoDot(AssemblerBuffer *buffer, Register rt, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXoForm(rt, ra, rb, 40, 1, 1));
}

static inline void asmNeg(AssemblerBuffer *buffer, Register rt, Register ra)
{
	asmPpcEmitWord(buffer, ppcXoForm(rt, ra, (Register) 0, 104, 0, 0));
}

static inline void asmNegoDot(AssemblerBuffer *buffer, Register rt, Register ra)
{
	asmPpcEmitWord(buffer, ppcXoForm(rt, ra, (Register) 0, 104, 1, 1));
}

static inline void asmMulld(AssemblerBuffer *buffer, Register rt, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXoForm(rt, ra, rb, 233, 0, 0));
}

static inline void asmMulldoDot(AssemblerBuffer *buffer, Register rt, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXoForm(rt, ra, rb, 233, 1, 1));
}

// divd of two IDENTICALLY-tagged ints yields the UNTAGGED quotient (the x64
// idiv trick); no trap on POWER for /0 or overflow, guards stay explicit.
static inline void asmDivd(AssemblerBuffer *buffer, Register rt, Register ra, Register rb)
{
	asmPpcEmitWord(buffer, ppcXoForm(rt, ra, rb, 489, 0, 0));
}

// ---- logical (X-form, RS in the high slot like asmOr) -----------------------------

static inline void asmAnd(AssemblerBuffer *buffer, Register ra, Register rs, Register rb)
{
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (rb & 31) << 11) | (28u << 1));
}

static inline void asmXor(AssemblerBuffer *buffer, Register ra, Register rs, Register rb)
{
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (rb & 31) << 11) | (316u << 1));
}

// andi. ALWAYS sets CR0, the tag-test workhorse (x64 `test reg, imm` + jcc
// becomes `andi. r0, reg, imm` + bne/beq; the result register is scratch r0).
static inline void asmAndiDot(AssemblerBuffer *buffer, Register ra, Register rs, uint32_t ui)
{
	ASSERT(ui <= 0xFFFF);
	asmPpcEmitWord(buffer, (28u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ui);
}

// addic. sets CR0, the dec-and-branch loop counter (x64 dec+jnz).
static inline void asmAddicDot(AssemblerBuffer *buffer, Register rt, Register ra, ptrdiff_t si)
{
	asmPpcEmitWord(buffer, ppcDForm(13, (uint8_t) rt, (uint8_t) ra, si));
}

// ---- shifts --------------------------------------------------------------------

// MD-form rldicl; srdi(ra,rs,n) == rldicl(ra,rs,64-n,n), clrldi(ra,rs,n) ==
// rldicl(ra,rs,0,n). Same rotated 6-bit mask encoding as asmRldicr.
static inline void asmRldicl(AssemblerBuffer *buffer, Register ra, Register rs, int sh, int mb)
{
	ASSERT(0 <= sh && sh <= 63 && 0 <= mb && mb <= 63);
	asmPpcEmitWord(buffer, (30u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (sh & 31) << 11)
		| (((((uint32_t) mb & 31) << 1) | ((uint32_t) mb >> 5)) << 5)
		| (((uint32_t) sh >> 5) << 1));
}

static inline void asmSrdi(AssemblerBuffer *buffer, Register ra, Register rs, int n)
{
	asmRldicl(buffer, ra, rs, (64 - n) & 63, n);
}

static inline void asmClrldi(AssemblerBuffer *buffer, Register ra, Register rs, int n)
{
	asmRldicl(buffer, ra, rs, 0, n);
}


// rotldi(ra,rs,n) == rldicl(ra,rs,n,0): pure 64-bit left rotate (the mask
// keeps every bit); rotrdi rotates right via 64-n. Used by the SmallFloat64
// immediate encode/decode in the inline Float intrinsic.
static inline void asmRotldi(AssemblerBuffer *buffer, Register ra, Register rs, int n)
{
	asmRldicl(buffer, ra, rs, n & 63, 0);
}

static inline void asmRotrdi(AssemblerBuffer *buffer, Register ra, Register rs, int n)
{
	asmRldicl(buffer, ra, rs, (64 - n) & 63, 0);
}

// GPR <-> FPR raw bit moves, ISA 2.07 (POWER8) VSX-with-GPR forms: FPR f is
// VSR f, so the TX/SX bit (word bit 31, our bit 0) stays 0 for f0-f31. Emit
// ONLY behind the isPower8 CPU gate; the baseline goes through memory.
// mtvsrd XT,RA (XO 179): moves RA into VSR XT.
static inline void asmMtvsrd(AssemblerBuffer *buffer, int frt, Register ra)
{
	asmPpcEmitWord(buffer, ppcXForm((Register) frt, ra, (Register) 0, 179));
}

// mfvsrd RA,XS (XO 51): moves VSR XS into RA.
static inline void asmMfvsrd(AssemblerBuffer *buffer, Register ra, int frs)
{
	asmPpcEmitWord(buffer, ppcXForm((Register) frs, ra, (Register) 0, 51));
}

// XS-form arithmetic shift right immediate (the untag shift).
static inline void asmSradi(AssemblerBuffer *buffer, Register ra, Register rs, int sh)
{
	ASSERT(0 <= sh && sh <= 63);
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (sh & 31) << 11)
		| (413u << 2) | (((uint32_t) sh >> 5) << 1));
}

// Register-amount shifts. POWER semantics: amount >= 64 yields 0 (x64 masks
// mod 64 instead, accepted divergence, DESIGN.md).
static inline void asmSld(AssemblerBuffer *buffer, Register ra, Register rs, Register rb)
{
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (rb & 31) << 11) | (27u << 1));
}

static inline void asmSrd(AssemblerBuffer *buffer, Register ra, Register rs, Register rb)
{
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (rb & 31) << 11) | (539u << 1));
}

static inline void asmSrad(AssemblerBuffer *buffer, Register ra, Register rs, Register rb)
{
	asmPpcEmitWord(buffer, (31u << 26) | ((uint32_t) (rs & 31) << 21)
		| ((uint32_t) (ra & 31) << 16) | ((uint32_t) (rb & 31) << 11) | (794u << 1));
}

// ---- XER / overflow protocol ------------------------------------------------------

enum { SPR_XER = 1 };

// Re-arm the sticky XER[SO] after a detected (or false-positive) overflow:
// li r0, 0; mtxer r0. Emitted on miss/dispatch paths only.
static inline void asmClearXerSo(AssemblerBuffer *buffer)
{
	asmLi(buffer, R0, 0);
	asmMtspr(buffer, SPR_XER, R0);
}

static inline void asmBso(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	asmBc(buffer, BO_IF_TRUE, CR_SO, label);
}

static inline void asmBns(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	asmBc(buffer, BO_IF_FALSE, CR_SO, label);
}

// ---- floating point (f0/f1 scratch; boxed Float intrinsics) ------------------------

static inline uint32_t ppcAForm(int frt, int fra, int frb, int frc, uint32_t xo)
{
	return (63u << 26) | ((uint32_t) (frt & 31) << 21) | ((uint32_t) (fra & 31) << 16)
		| ((uint32_t) (frb & 31) << 11) | ((uint32_t) (frc & 31) << 6) | (xo << 1);
}

static inline void asmFadd(AssemblerBuffer *buffer, int frt, int fra, int frb)
{
	asmPpcEmitWord(buffer, ppcAForm(frt, fra, frb, 0, 21));
}

static inline void asmFsub(AssemblerBuffer *buffer, int frt, int fra, int frb)
{
	asmPpcEmitWord(buffer, ppcAForm(frt, fra, frb, 0, 20));
}

static inline void asmFdiv(AssemblerBuffer *buffer, int frt, int fra, int frb)
{
	asmPpcEmitWord(buffer, ppcAForm(frt, fra, frb, 0, 18));
}

// fmul uses the FRC field (FRB must be 0).
static inline void asmFmul(AssemblerBuffer *buffer, int frt, int fra, int frc)
{
	asmPpcEmitWord(buffer, ppcAForm(frt, fra, 0, frc, 25));
}

// fcmpu crf, fra, frb, the unordered (NaN) outcome lands in the crf's SO
// bit (BI = crf*4 + CR_SO): a DEDICATED bit, unlike x64's parity-flag dance.
static inline void asmFcmpu(AssemblerBuffer *buffer, int crf, int fra, int frb)
{
	ASSERT(0 <= crf && crf <= 7);
	asmPpcEmitWord(buffer, (63u << 26) | ((uint32_t) crf << 23)
		| ((uint32_t) (fra & 31) << 16) | ((uint32_t) (frb & 31) << 11));
}

// Unconditional trap (tw 31,0,0), the x64 int3 analog (InterruptPrimitive).
static inline void asmTrap(AssemblerBuffer *buffer)
{
	asmPpcEmitWord(buffer, 0x7FE00008);
}

// ---- position capture ----------------------------------------------------------

// bcl 20,31,$+4, the PIC "load next address into LR" idiom (link-stack
// neutral form); pairs with mflr to materialize the current code position
// (x64: lea rip+0). LR is dead inside JIT method bodies (DESIGN.md).
static inline void asmBclNext(AssemblerBuffer *buffer)
{
	asmPpcEmitWord(buffer, (16u << 26) | (20u << 21) | (31u << 16) | 4u | 1u);
}

// ---- tagged-base doubleword access -----------------------------------------------
// DS-form (ld/std) encodes only 4-aligned displacements, but the VM
// constantly addresses fields off TAGGED heap pointers (base+1, field
// offsets folded as offsetof-1 ≡ 3 mod 4, x64's free trick). The ACCESS is
// aligned; only the encoding isn't. These helpers keep the x64 addressing
// idiom: aligned displacements emit one ld/std, tag-folded ones untag the
// base into the dedicated TMP2 first (dst may alias base; TMP2 is owned by
// this helper family and must not be live at call sites).

static inline void asmLdT(AssemblerBuffer *buffer, Register dst, ptrdiff_t disp, Register base)
{
	if ((disp & 3) == 0) {
		asmLd(buffer, dst, disp, base);
	} else {
		ASSERT((disp & 3) == 3); // tag-folded field offset
		asmAddi(buffer, TMP2, base, -1);
		asmLd(buffer, dst, disp + 1, TMP2);
	}
}

static inline void asmStdT(AssemblerBuffer *buffer, Register src, ptrdiff_t disp, Register base)
{
	if ((disp & 3) == 0) {
		asmStd(buffer, src, disp, base);
	} else {
		ASSERT((disp & 3) == 3);
		ASSERT(src != TMP2);
		asmAddi(buffer, TMP2, base, -1);
		asmStd(buffer, src, disp + 1, TMP2);
	}
}

// ---- composite helpers (the x64 push/pop/call vocabulary) ----------------------

// push: ONE instruction (atomic decrement+store, nothing below r1 is ever
// live, signal-safe like x64 push).
static inline void asmPush(AssemblerBuffer *buffer, Register src)
{
	asmStdu(buffer, src, -8, R1);
}

static inline void asmPop(AssemblerBuffer *buffer, Register dst)
{
	asmLd(buffer, dst, 0, R1);
	asmAddi(buffer, R1, R1, 8);
}

static inline void asmDropStack(AssemblerBuffer *buffer, int slots)
{
	asmAddi(buffer, R1, R1, slots * 8);
}

// Indirect call/jump through CTR. WARNING: bctrl CLOBBERS LR, see the LR
// discipline in DESIGN.md (method bodies: LR dead; framed primitives pushed
// LR at entry; generateCCall saves/restores it itself).
static inline void asmCallReg(AssemblerBuffer *buffer, Register target)
{
	asmMtctr(buffer, target);
	asmBctrl(buffer);
}

static inline void asmJumpReg(AssemblerBuffer *buffer, Register target)
{
	asmMtctr(buffer, target);
	asmBctr(buffer);
}

// TLS load delegate, same pattern as x64 (bound in the selected ABI's
// Abi<Abi>Bind.c to gPpc64leAbi->emitLoadTls). WARNING: only meaningful in a
// ppc64le BUILD. Host-compiled golden code must call the elfv2 instance hook
// directly, since in a foreign-arch binary this name resolves to the HOST
// backend's TLS emitter.
void asmLoadTls(AssemblerBuffer *buffer, Register dst, ptrdiff_t tpoff);

#endif
