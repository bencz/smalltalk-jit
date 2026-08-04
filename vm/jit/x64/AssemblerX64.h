#ifndef ASSEMBLER_X64_H
#define ASSEMBLER_X64_H

// x86-64 instruction encoding, and only the instructions this JIT emits.
//
// Deliberately small. A general assembler is a lot of surface to get subtly
// wrong, and every encoding here is exercised by generated code that runs in
// the self-test, so nothing sits untested. Adding an instruction means adding
// it to the test that executes it.

#include "jit/CodeBuffer.h"
#include <stdint.h>

typedef enum {
	RAX = 0, RCX = 1, RDX = 2, RBX = 3,
	RSP = 4, RBP = 5, RSI = 6, RDI = 7,
	R8 = 8, R9 = 9, R10 = 10, R11 = 11,
	R12 = 12, R13 = 13, R14 = 14, R15 = 15,
	NO_REGISTER = -1,
} Register;

// The SSE register file, numbered from zero IN ITS OWN SPACE.
//
// A SEPARATE ENUM and not more entries in the one above, because they are a
// separate BANK: the SSA backend allocates the two independently, so register 3
// means RBX in one and XMM3 in the other, and jit/Abi.h says exactly that where
// it calls the numbers backend-local. Folding them into one enum would make the
// overlap look like a collision and tempt a reader to renumber one of them,
// which is the sort of change that produces code that assembles and computes
// with the wrong file.
typedef enum {
	XMM0 = 0, XMM1 = 1, XMM2 = 2, XMM3 = 3,
	XMM4 = 4, XMM5 = 5, XMM6 = 6, XMM7 = 7,
	XMM8 = 8, XMM9 = 9, XMM10 = 10, XMM11 = 11,
	XMM12 = 12, XMM13 = 13, XMM14 = 14, XMM15 = 15,
} XmmRegister;

typedef enum {
	COND_OVERFLOW = 0x0, COND_NO_OVERFLOW = 0x1,
	COND_BELOW = 0x2, COND_ABOVE_EQUAL = 0x3,
	COND_EQUAL = 0x4, COND_NOT_EQUAL = 0x5,
	COND_BELOW_EQUAL = 0x6, COND_ABOVE = 0x7,
	COND_SIGN = 0x8, COND_NO_SIGN = 0x9,
	// PARITY is how an SSE comparison reports NaN: ucomisd sets it instead of
	// raising, and every ordered answer about a NaN is false. A float fast path
	// that ignored it would answer `NaN < 1.0` with whatever the other flags
	// happened to hold.
	COND_PARITY = 0xA, COND_NO_PARITY = 0xB,
	COND_LESS = 0xC, COND_GREATER_EQUAL = 0xD,
	COND_LESS_EQUAL = 0xE, COND_GREATER = 0xF,
} Condition;


// The opposite test. x86 numbers its condition codes in PAIRS, each condition
// immediately followed by its negation, so flipping the low bit is the whole
// operation -- which is exactly why every pair above is written on one line.
// A caller that has a condition in a variable and needs to branch the other way
// asks for it here rather than carrying a second column in its table.
static inline Condition invertCondition(Condition condition)
{
	return (Condition) (condition ^ 1);
}


// A forward reference: emitted with a placeholder displacement, patched when
// the label is bound. A LIST of references, not one slot: the old assembler
// allowed exactly one forward reference per label and silently produced a wrong
// displacement for the second, which shows up as a jump into the middle of an
// instruction.
#define LABEL_MAX_REFERENCES 64

typedef struct X64Label {
	_Bool bound;
	size_t offset;
	size_t references[LABEL_MAX_REFERENCES];
	size_t referenceCount;
} X64Label;


// REX.W is always set: every value in this VM is 64 bits wide, so there is no
// 32-bit operand path to get wrong.
static inline void emitRexW(CodeBuffer *buffer, Register reg, Register rm)
{
	uint8_t rex = 0x48;
	if (reg >= R8) { rex |= 0x04; }
	if (rm >= R8) { rex |= 0x01; }
	emit8(buffer, rex);
}


static inline void emitModRmReg(CodeBuffer *buffer, Register reg, Register rm)
{
	emit8(buffer, (uint8_t) (0xC0 | ((reg & 7) << 3) | (rm & 7)));
}


// [base + displacement]. RSP needs a SIB byte and RBP needs an explicit zero
// displacement; both are encoding quirks rather than choices, and both are why
// this helper exists instead of open-coded ModRM everywhere.
static inline void emitModRmMem(CodeBuffer *buffer, Register reg, Register base,
	int32_t displacement)
{
	uint8_t mod;
	if (displacement == 0 && (base & 7) != (RBP & 7)) {
		mod = 0x00;
	} else if (displacement >= -128 && displacement <= 127) {
		mod = 0x40;
	} else {
		mod = 0x80;
	}
	emit8(buffer, (uint8_t) (mod | ((reg & 7) << 3) | (base & 7)));
	if ((base & 7) == (RSP & 7)) {
		emit8(buffer, 0x24); // SIB: base only, no index
	}
	if (mod == 0x40) {
		emit8(buffer, (uint8_t) displacement);
	} else if (mod == 0x80) {
		emit32(buffer, (uint32_t) displacement);
	}
}


// ---- moves -----------------------------------------------------------------

static inline void asmMovRegReg(CodeBuffer *buffer, Register dst, Register src)
{
	emitRexW(buffer, src, dst);
	emit8(buffer, 0x89);
	emitModRmReg(buffer, src, dst);
}


static inline void asmMovRegImm64(CodeBuffer *buffer, Register dst, uint64_t value)
{
	uint8_t rex = 0x48;
	if (dst >= R8) { rex |= 0x01; }
	emit8(buffer, rex);
	emit8(buffer, (uint8_t) (0xB8 + (dst & 7)));
	emit64(buffer, value);
}


// dst = [base + displacement]
static inline void asmMovRegMem(CodeBuffer *buffer, Register dst, Register base,
	int32_t displacement)
{
	emitRexW(buffer, dst, base);
	emit8(buffer, 0x8B);
	emitModRmMem(buffer, dst, base, displacement);
}


// [base + displacement] = src
static inline void asmMovMemReg(CodeBuffer *buffer, Register base,
	int32_t displacement, Register src)
{
	emitRexW(buffer, src, base);
	emit8(buffer, 0x89);
	emitModRmMem(buffer, src, base, displacement);
}


// dst = base + displacement, computed without touching memory. How a compiled
// send hands the runtime the ADDRESS of a frame slot.
static inline void asmLeaRegMem(CodeBuffer *buffer, Register dst, Register base,
	int32_t displacement)
{
	emitRexW(buffer, dst, base);
	emit8(buffer, 0x8D);
	emitModRmMem(buffer, dst, base, displacement);
}


// ---- arithmetic ------------------------------------------------------------

static inline void asmAddRegReg(CodeBuffer *buffer, Register dst, Register src)
{
	emitRexW(buffer, src, dst);
	emit8(buffer, 0x01);
	emitModRmReg(buffer, src, dst);
}


static inline void asmSubRegReg(CodeBuffer *buffer, Register dst, Register src)
{
	emitRexW(buffer, src, dst);
	emit8(buffer, 0x29);
	emitModRmReg(buffer, src, dst);
}


static inline void asmAddRegImm32(CodeBuffer *buffer, Register dst, int32_t value)
{
	emitRexW(buffer, (Register) 0, dst);
	emit8(buffer, 0x81);
	emitModRmReg(buffer, (Register) 0, dst);
	emit32(buffer, (uint32_t) value);
}


// [base + displacement] += value, sixty-four bits, WITHOUT a register.
//
// The inline cache bumps two counters per hit (the site's total and the way's),
// and doing it through a register would be three instructions each plus a
// scratch that has to survive the rest of the sequence. The counters are
// uint64_t, so this is the REX.W form: a 32-bit add would wrap silently at four
// billion sends, which a long-running program reaches.
static inline void asmAddMemImm32(CodeBuffer *buffer, Register base,
	int32_t displacement, int32_t value)
{
	emitRexW(buffer, (Register) 0, base);
	emit8(buffer, 0x81);
	emitModRmMem(buffer, (Register) 0, base, displacement);
	emit32(buffer, (uint32_t) value);
}


static inline void asmSubRegImm32(CodeBuffer *buffer, Register dst, int32_t value)
{
	emitRexW(buffer, (Register) 5, dst);
	emit8(buffer, 0x81);
	emitModRmReg(buffer, (Register) 5, dst);
	emit32(buffer, (uint32_t) value);
}


static inline void asmCmpRegReg(CodeBuffer *buffer, Register a, Register b)
{
	emitRexW(buffer, b, a);
	emit8(buffer, 0x39);
	emitModRmReg(buffer, b, a);
}


static inline void asmCmpRegImm32(CodeBuffer *buffer, Register reg, int32_t value)
{
	emitRexW(buffer, (Register) 7, reg);
	emit8(buffer, 0x81);
	emitModRmReg(buffer, (Register) 7, reg);
	emit32(buffer, (uint32_t) value);
}


static inline void asmTestRegImm32(CodeBuffer *buffer, Register reg, int32_t value)
{
	emitRexW(buffer, (Register) 0, reg);
	emit8(buffer, 0xF7);
	emitModRmReg(buffer, (Register) 0, reg);
	emit32(buffer, (uint32_t) value);
}


static inline void asmOrRegReg(CodeBuffer *buffer, Register dst, Register src)
{
	emitRexW(buffer, src, dst);
	emit8(buffer, 0x09);
	emitModRmReg(buffer, src, dst);
}


static inline void asmSarRegImm8(CodeBuffer *buffer, Register reg, uint8_t bits)
{
	emitRexW(buffer, (Register) 7, reg);
	emit8(buffer, 0xC1);
	emitModRmReg(buffer, (Register) 7, reg);
	emit8(buffer, bits);
}


static inline void asmShlRegImm8(CodeBuffer *buffer, Register reg, uint8_t bits)
{
	emitRexW(buffer, (Register) 4, reg);
	emit8(buffer, 0xC1);
	emitModRmReg(buffer, (Register) 4, reg);
	emit8(buffer, bits);
}


// LOGICAL right shift, which is a different instruction from asmSarRegImm8 above
// and not a stylistic choice: the SmallFloat64 payload is an unsigned bit
// pattern, and shifting its top bit in as a sign would corrupt every value in
// the upper half of the encoding's range (core/Object.h).
static inline void asmShrRegImm8(CodeBuffer *buffer, Register reg, uint8_t bits)
{
	emitRexW(buffer, (Register) 5, reg);
	emit8(buffer, 0xC1);
	emitModRmReg(buffer, (Register) 5, reg);
	emit8(buffer, bits);
}


// Rotates. The SmallFloat64 encoding is a one-bit rotation of the IEEE pattern,
// so these two ARE the encode and decode step, not an optimization of one: the
// C spells it `(bits >> 1) | (bits << 63)` because ISO C has no rotate.
static inline void asmRolRegImm8(CodeBuffer *buffer, Register reg, uint8_t bits)
{
	emitRexW(buffer, (Register) 0, reg);
	emit8(buffer, 0xC1);
	emitModRmReg(buffer, (Register) 0, reg);
	emit8(buffer, bits);
}


static inline void asmRorRegImm8(CodeBuffer *buffer, Register reg, uint8_t bits)
{
	emitRexW(buffer, (Register) 1, reg);
	emit8(buffer, 0xC1);
	emitModRmReg(buffer, (Register) 1, reg);
	emit8(buffer, bits);
}


// ---- 32-bit forms, for reading the low half of an object header -------------
//
// No REX.W: these operate on 32 bits and zero-extend into the full register,
// which is exactly what is wanted when pulling a bitfield out of a header.

static inline void asmMov32RegMem(CodeBuffer *buffer, Register dst, Register base,
	int32_t displacement)
{
	if (dst >= R8 || base >= R8) {
		uint8_t rex = 0x40;
		if (dst >= R8) { rex |= 0x04; }
		if (base >= R8) { rex |= 0x01; }
		emit8(buffer, rex);
	}
	emit8(buffer, 0x8B);
	emitModRmMem(buffer, dst, base, displacement);
}


// [base + displacement] = src, THIRTY-TWO BITS. The inline cache writes the
// argument's class index into its way with it: the field is uint32_t, and a
// 64-bit store would take the neighbouring field with it.
static inline void asmMov32MemReg(CodeBuffer *buffer, Register base,
	int32_t displacement, Register src)
{
	if (src >= R8 || base >= R8) {
		uint8_t rex = 0x40;
		if (src >= R8) { rex |= 0x04; }
		if (base >= R8) { rex |= 0x01; }
		emit8(buffer, rex);
	}
	emit8(buffer, 0x89);
	emitModRmMem(buffer, src, base, displacement);
}


static inline void asmAnd32RegImm32(CodeBuffer *buffer, Register reg, uint32_t value)
{
	if (reg >= R8) { emit8(buffer, 0x41); }
	emit8(buffer, 0x81);
	emitModRmReg(buffer, (Register) 4, reg);
	emit32(buffer, value);
}


static inline void asmCmp32RegImm32(CodeBuffer *buffer, Register reg, uint32_t value)
{
	if (reg >= R8) { emit8(buffer, 0x41); }
	emit8(buffer, 0x81);
	emitModRmReg(buffer, (Register) 7, reg);
	emit32(buffer, value);
}


// ---- 32-bit compare against an immediate ------------------------------------
//
// The inline-cache guard, and the reason the object header carries a class
// INDEX rather than a pointer (ADR 0005): `cmp dword [obj], imm32` compares the
// low half of the header, where the 22-bit class index lives, without loading
// the class and without touching a second cache line.
// The SIXTY-FOUR bit form, which the one below is deliberately not: that one
// compares a 32-bit field and this one compares a whole word. A caller testing a
// uint64 counter with the 32-bit version would be testing the low half and
// answering differently every time the counter crossed 2^32.
static inline void asmCmpMemImm32(CodeBuffer *buffer, Register base,
	int32_t displacement, int32_t value)
{
	emitRexW(buffer, (Register) 7, base);
	emit8(buffer, 0x81);
	emitModRmMem(buffer, (Register) 7, base, displacement);
	emit32(buffer, (uint32_t) value);
}


static inline void asmCmpMem32Imm32(CodeBuffer *buffer, Register base,
	int32_t displacement, uint32_t value)
{
	if (base >= R8) {
		emit8(buffer, 0x41); // REX.B only: this is a 32-bit compare
	}
	emit8(buffer, 0x81);
	emitModRmMem(buffer, (Register) 7, base, displacement);
	emit32(buffer, value);
}


// ---- stack -----------------------------------------------------------------

static inline void asmPush(CodeBuffer *buffer, Register reg)
{
	if (reg >= R8) { emit8(buffer, 0x41); }
	emit8(buffer, (uint8_t) (0x50 + (reg & 7)));
}


static inline void asmPop(CodeBuffer *buffer, Register reg)
{
	if (reg >= R8) { emit8(buffer, 0x41); }
	emit8(buffer, (uint8_t) (0x58 + (reg & 7)));
}


static inline void asmRet(CodeBuffer *buffer)
{
	emit8(buffer, 0xC3);
}


static inline void asmCallReg(CodeBuffer *buffer, Register reg)
{
	if (reg >= R8) { emit8(buffer, 0x41); }
	emit8(buffer, 0xFF);
	emitModRmReg(buffer, (Register) 2, reg);
}


// ---- labels and branches ---------------------------------------------------

// ---- what the SSA backend needs and the template compiler never did --------
//
// Tier 1 computes nothing: every bytecode is a load, a store, a compare or a
// call. Tier 2 keeps values in registers and does arithmetic on them, so it
// needs the instructions that do arithmetic.

// dst = dst * src. Signed, and the two-operand form, so the overflow flags are
// set from the low 64 bits -- which is all this backend looks at, because a
// SmallInteger result that does not fit is the Smalltalk fallback's problem.
static inline void asmImulRegReg(CodeBuffer *buffer, Register dst, Register src)
{
	emitRexW(buffer, dst, src);
	emit8(buffer, 0x0F);
	emit8(buffer, 0xAF);
	emitModRmReg(buffer, dst, src);
}


// Sign-extend RAX into RDX:RAX. The instruction idiv REQUIRES this first, and
// forgetting it is not a crash: it divides by whatever RDX held and answers a
// plausible wrong number.
static inline void asmCqo(CodeBuffer *buffer)
{
	emit8(buffer, 0x48);
	emit8(buffer, 0x99);
}


// RDX:RAX / src, quotient in RAX and remainder in RDX. Both are clobbered,
// which is why the allocator is told about them (jit/RegAlloc.c addClobbers).
static inline void asmIdivReg(CodeBuffer *buffer, Register src)
{
	emitRexW(buffer, (Register) 0, src);
	emit8(buffer, 0xF7);
	emitModRmReg(buffer, (Register) 7, src);
}


static inline void asmNegReg(CodeBuffer *buffer, Register dst)
{
	emitRexW(buffer, (Register) 0, dst);
	emit8(buffer, 0xF7);
	emitModRmReg(buffer, (Register) 3, dst);
}


static inline void asmXorRegReg(CodeBuffer *buffer, Register dst, Register src)
{
	emitRexW(buffer, src, dst);
	emit8(buffer, 0x31);
	emitModRmReg(buffer, src, dst);
}


static inline void asmAndRegReg(CodeBuffer *buffer, Register dst, Register src)
{
	emitRexW(buffer, src, dst);
	emit8(buffer, 0x21);
	emitModRmReg(buffer, src, dst);
}


// Set the low byte of `reg` to 0 or 1 by condition, then widen it.
//
// THE REX PREFIX IS NOT OPTIONAL HERE even when no extended register is
// involved. Without it, encoding 4 to 7 in the byte-register space means AH,
// CH, DH and BH rather than SPL, BPL, SIL and DIL, so `setcc sil` silently
// becomes `setcc dh` and writes the wrong half of a different register. That is
// the exact family of bug this repository already caught once, in asmTestbImm
// encoding BH for RDI.
static inline void asmSetcc(CodeBuffer *buffer, Condition condition, Register reg)
{
	if (reg >= 4) {
		emit8(buffer, (uint8_t) (0x40 | (reg >= R8 ? 0x01 : 0x00)));
	}
	emit8(buffer, 0x0F);
	emit8(buffer, (uint8_t) (0x90 + condition));
	emit8(buffer, (uint8_t) (0xC0 | (reg & 7)));
}


// Zero-extend the low byte, which is what turns a setcc into a 0 or a 1 that
// the rest of the backend can do arithmetic with.
static inline void asmMovzxByte(CodeBuffer *buffer, Register dst, Register src)
{
	uint8_t rex = 0x48;
	if (dst >= R8) { rex |= 0x04; }
	if (src >= R8) { rex |= 0x01; }
	emit8(buffer, rex);
	emit8(buffer, 0x0F);
	emit8(buffer, 0xB6);
	emitModRmReg(buffer, dst, src);
}


// ---- SSE, scalar double ----------------------------------------------------
//
// The mandatory prefix comes FIRST and REX comes after it, before the 0F. The
// other order assembles into something else entirely, and it is the single
// easiest thing to get wrong in SSE encoding.
static inline void emitSseRex(CodeBuffer *buffer, uint8_t prefix, _Bool wide,
	uint8_t reg, uint8_t rm)
{
	emit8(buffer, prefix);
	uint8_t rex = (uint8_t) (0x40 | (wide ? 0x08 : 0x00)
		| (reg >= 8 ? 0x04 : 0x00) | (rm >= 8 ? 0x01 : 0x00));
	if (rex != 0x40) {
		emit8(buffer, rex);
	}
	emit8(buffer, 0x0F);
}


static inline void asmSseRegReg(CodeBuffer *buffer, uint8_t prefix,
	uint8_t opcode, XmmRegister dst, XmmRegister src)
{
	emitSseRex(buffer, prefix, 0, (uint8_t) dst, (uint8_t) src);
	emit8(buffer, opcode);
	emitModRmReg(buffer, (Register) dst, (Register) src);
}


#define asmMovsdRegReg(b, d, s) asmSseRegReg((b), 0xF2, 0x10, (d), (s))
#define asmAddsd(b, d, s)       asmSseRegReg((b), 0xF2, 0x58, (d), (s))
#define asmMulsd(b, d, s)       asmSseRegReg((b), 0xF2, 0x59, (d), (s))
#define asmSubsd(b, d, s)       asmSseRegReg((b), 0xF2, 0x5C, (d), (s))
#define asmDivsd(b, d, s)       asmSseRegReg((b), 0xF2, 0x5E, (d), (s))
#define asmSqrtsd(b, d, s)      asmSseRegReg((b), 0xF2, 0x51, (d), (s))
// UNORDERED compare, which is the right one for IEEE doubles: it sets the
// parity flag for NaN instead of raising, so a comparison against NaN answers
// false rather than trapping.
#define asmUcomisd(b, a, x)     asmSseRegReg((b), 0x66, 0x2E, (a), (x))


static inline void asmMovsdRegMem(CodeBuffer *buffer, XmmRegister dst,
	Register base, int32_t displacement)
{
	emitSseRex(buffer, 0xF2, 0, (uint8_t) dst, (uint8_t) base);
	emit8(buffer, 0x10);
	emitModRmMem(buffer, (Register) dst, base, displacement);
}


static inline void asmMovsdMemReg(CodeBuffer *buffer, Register base,
	int32_t displacement, XmmRegister src)
{
	emitSseRex(buffer, 0xF2, 0, (uint8_t) src, (uint8_t) base);
	emit8(buffer, 0x11);
	emitModRmMem(buffer, (Register) src, base, displacement);
}


// Integer to double, and double to integer. NUMERIC conversions: 1 becomes 1.0.
// Not to be confused with the two below, which reinterpret the same bits.
static inline void asmCvtsi2sd(CodeBuffer *buffer, XmmRegister dst, Register src)
{
	emitSseRex(buffer, 0xF2, 1, (uint8_t) dst, (uint8_t) src);
	emit8(buffer, 0x2A);
	emitModRmReg(buffer, (Register) dst, src);
}


static inline void asmCvttsd2si(CodeBuffer *buffer, Register dst, XmmRegister src)
{
	emitSseRex(buffer, 0xF2, 1, (uint8_t) dst, (uint8_t) src);
	emit8(buffer, 0x2C);
	emitModRmReg(buffer, dst, (Register) src);
}


// The same eight bytes, moved between the files. BIT REINTERPRETATION, which is
// what boxing a double is: 1.0 becomes 4607182418800017408, not 1.
static inline void asmMovqXmmFromGpr(CodeBuffer *buffer, XmmRegister dst,
	Register src)
{
	emitSseRex(buffer, 0x66, 1, (uint8_t) dst, (uint8_t) src);
	emit8(buffer, 0x6E);
	emitModRmReg(buffer, (Register) dst, src);
}


static inline void asmMovqGprFromXmm(CodeBuffer *buffer, Register dst,
	XmmRegister src)
{
	emitSseRex(buffer, 0x66, 1, (uint8_t) src, (uint8_t) dst);
	emit8(buffer, 0x7E);
	emitModRmReg(buffer, (Register) src, dst);
}


static inline void asmInitLabel(X64Label *label)
{
	label->bound = 0;
	label->offset = 0;
	label->referenceCount = 0;
}


static inline void asmAddReference(X64Label *label, size_t offset)
{
	// A list, not a single slot. The old assembler allowed exactly one forward
	// reference per label and silently produced a wrong displacement for the
	// second, which is a class of bug that only shows up as a jump into the
	// middle of an instruction.
	ASSERT(label->referenceCount < LABEL_MAX_REFERENCES);
	label->references[label->referenceCount++] = offset;
}


static inline void asmBind(CodeBuffer *buffer, X64Label *label)
{
	ASSERT(!label->bound);
	label->bound = 1;
	label->offset = buffer->size;
	for (size_t i = 0; i < label->referenceCount; i++) {
		size_t site = label->references[i];
		int32_t displacement = (int32_t) (label->offset - (site + 4));
		memcpy(buffer->bytes + site, &displacement, 4);
	}
	label->referenceCount = 0;
}


static inline void asmJmp(CodeBuffer *buffer, X64Label *label)
{
	emit8(buffer, 0xE9);
	if (label->bound) {
		emit32(buffer, (uint32_t) (int32_t) (label->offset - (buffer->size + 4)));
	} else {
		asmAddReference(label, buffer->size);
		emit32(buffer, 0);
	}
}


static inline void asmJcc(CodeBuffer *buffer, Condition condition, X64Label *label)
{
	emit8(buffer, 0x0F);
	emit8(buffer, (uint8_t) (0x80 + condition));
	if (label->bound) {
		emit32(buffer, (uint32_t) (int32_t) (label->offset - (buffer->size + 4)));
	} else {
		asmAddReference(label, buffer->size);
		emit32(buffer, 0);
	}
}

#endif
