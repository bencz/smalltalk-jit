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

typedef enum {
	COND_OVERFLOW = 0x0, COND_NO_OVERFLOW = 0x1,
	COND_BELOW = 0x2, COND_ABOVE_EQUAL = 0x3,
	COND_EQUAL = 0x4, COND_NOT_EQUAL = 0x5,
	COND_BELOW_EQUAL = 0x6, COND_ABOVE = 0x7,
	COND_SIGN = 0x8, COND_NO_SIGN = 0x9,
	COND_LESS = 0xC, COND_GREATER_EQUAL = 0xD,
	COND_LESS_EQUAL = 0xE, COND_GREATER = 0xF,
} Condition;

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
