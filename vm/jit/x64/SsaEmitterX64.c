// The x86-64 half of the SSA backend. Every register name in tier 2 is here.
//
// TWO SCRATCH REGISTERS, and they are the ones the ABI tables already hold back
// from the allocator: RAX and RCX in the integer file, XMM0 in the float file
// (vm/jit/x64/abi/*/). Nothing here may use anything else that is not an
// operand, because everything else was handed out.
//
// WHY A SCRATCH IS NEEDED AT ALL. This LIR is three-address and x86 is
// two-address, so `d := a + b` is a move and an add. The naive lowering, `mov d,
// a; add d, b`, is WRONG whenever d and b are the same register: the move
// destroys b before the add reads it. Staging through the scratch is correct for
// every combination, and costs one move that a peephole pass can take back when
// there is one to measure.

#include "jit/SsaEmitter.h"
#include "jit/x64/AssemblerX64.h"
#include "jit/Jit.h"
#include "jit/Deopt.h"
#include "core/Assert.h"
#include "core/Thread.h"
#include "memory/Heap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The ABI table, defined one directory down beside the register sets it
// describes. Declared rather than included, because Abi.h is the neutral
// SHAPE and this is one instance of it.
extern const Abi gAbiX64SysV;

#define X64_SCRATCH_A RAX
#define X64_SCRATCH_B RCX
#define X64_FRAME RBP
#define X64_FSCRATCH XMM0

typedef struct {
	X64Label **labels;
	uint32_t count, capacity;
} EmitterState;


// The frame contract, shared with tier 1 word for word: slot i at
// [rbp - 8*(i+1)]. See jit/Lir.h for why tier 2 keeps it rather than choosing
// its own.
static int32_t slotOffset(uint16_t index)
{
	return -(int32_t) ((index + 1) * sizeof(Value));
}


static EmitterState *state(SsaEmitter *emitter)
{
	return (EmitterState *) ssaEmitterState(emitter);
}


static Register intReg(int16_t reg)
{
	ASSERT(reg >= 0);
	return (Register) reg;
}


static XmmRegister floatReg(int16_t reg)
{
	ASSERT(reg >= 0);
	return (XmmRegister) reg;
}


static Condition conditionFor(uint8_t lir)
{
	switch ((LirCondition) lir) {
	case LIR_CMP_EQ: return COND_EQUAL;
	case LIR_CMP_NE: return COND_NOT_EQUAL;
	case LIR_CMP_LT: return COND_LESS;
	case LIR_CMP_LE: return COND_LESS_EQUAL;
	case LIR_CMP_GT: return COND_GREATER;
	case LIR_CMP_GE: return COND_GREATER_EQUAL;
	case LIR_CMP_ULT: return COND_BELOW;
	default: return COND_ABOVE_EQUAL;
	}
}


// A float compare answers UNSIGNED conditions, and that is not a detail:
// ucomisd reports its result in the carry and zero flags, so the signed
// conditions read the wrong bits. Using COND_LESS after a ucomisd is a
// comparison that is wrong for exactly half its inputs.
static Condition floatConditionFor(uint8_t lir)
{
	switch ((LirCondition) lir) {
	case LIR_CMP_EQ: return COND_EQUAL;
	case LIR_CMP_NE: return COND_NOT_EQUAL;
	case LIR_CMP_LT: return COND_BELOW;
	case LIR_CMP_LE: return COND_BELOW_EQUAL;
	case LIR_CMP_GT: return COND_ABOVE;
	default: return COND_ABOVE_EQUAL;
	}
}


// ---- the operations --------------------------------------------------------

static void x64SsaBegin(SsaEmitter *emitter)
{
	EmitterState *s = state(emitter);
	s->labels = NULL;
	s->count = s->capacity = 0;
}


static void x64SsaEnd(SsaEmitter *emitter)
{
	EmitterState *s = state(emitter);
	for (uint32_t i = 0; i < s->count; i++) {
		free(s->labels[i]);
	}
	free(s->labels);
	s->labels = NULL;
	s->count = s->capacity = 0;
}


static SsaLabel *x64SsaNewLabel(SsaEmitter *emitter)
{
	EmitterState *s = state(emitter);
	if (s->count == s->capacity) {
		s->capacity = s->capacity == 0 ? 32 : s->capacity * 2;
		s->labels = realloc(s->labels, s->capacity * sizeof(X64Label *));
		ASSERT(s->labels != NULL);
	}
	// Each label on its own, because a branch holds a pointer to one and the
	// pool grows.
	X64Label *label = malloc(sizeof(X64Label));
	ASSERT(label != NULL);
	asmInitLabel(label);
	s->labels[s->count++] = label;
	return (SsaLabel *) label;
}


static void x64SsaBind(SsaEmitter *emitter, SsaLabel *label)
{
	asmBind(ssaEmitterBuffer(emitter), (X64Label *) label);
}


static void x64SsaJump(SsaEmitter *emitter, SsaLabel *label)
{
	asmJmp(ssaEmitterBuffer(emitter), (X64Label *) label);
}


// How many slots the frame reserves beyond the ones the LIR names: one per
// callee-saved register the allocation used.
static uint16_t savedSlotCount(const LirFunction *function)
{
	uint16_t count = 0;
	for (int bank = 0; bank < LIR_BANK_COUNT; bank++) {
		for (int reg = 0; reg < 32; reg++) {
			count += (function->calleeSavedUsed[bank] >> reg) & 1;
		}
	}
	return count;
}


// Save or restore the callee-saved registers the allocation used, INSIDE the
// frame rather than by pushing.
//
// Pushing after the frame pointer is set would put them between rbp and the
// slots, and the slots are addressed from rbp, so the frame would overlap them.
// Reserving room at the top of the frame keeps one rule for every address in
// the method.
static void emitSavedRegisters(SsaEmitter *emitter, _Bool save)
{
	CodeBuffer *buffer = ssaEmitterBuffer(emitter);
	const LirFunction *function = ssaEmitterFunction(emitter);
	uint16_t slot = function->frameSlots;
	for (int bank = 0; bank < LIR_BANK_COUNT; bank++) {
		for (int reg = 0; reg < 32; reg++) {
			if (((function->calleeSavedUsed[bank] >> reg) & 1) == 0) {
				continue;
			}
			if (bank == LIR_BANK_INT) {
				if (save) {
					asmMovMemReg(buffer, X64_FRAME, slotOffset(slot),
						intReg((int16_t) reg));
				} else {
					asmMovRegMem(buffer, intReg((int16_t) reg), X64_FRAME,
						slotOffset(slot));
				}
			} else {
				if (save) {
					asmMovsdMemReg(buffer, X64_FRAME, slotOffset(slot),
						floatReg((int16_t) reg));
				} else {
					asmMovsdRegMem(buffer, floatReg((int16_t) reg), X64_FRAME,
						slotOffset(slot));
				}
			}
			slot++;
		}
	}
}


static void x64SsaPrologue(SsaEmitter *emitter, Value nilValue)
{
	CodeBuffer *buffer = ssaEmitterBuffer(emitter);
	const LirFunction *function = ssaEmitterFunction(emitter);
	const Abi *abi = ssaEmitterAbi(emitter);

	asmPush(buffer, X64_FRAME);
	asmMovRegReg(buffer, X64_FRAME, RSP);
	uint16_t slots = (uint16_t) (function->frameSlots + savedSlotCount(function));
	uint16_t unit = (uint16_t) (abi->stackAlignment / sizeof(Value));
	int32_t frameBytes = (int32_t) (((slots + unit - 1) / unit) * unit
		* sizeof(Value));
	asmSubRegImm32(buffer, RSP, frameBytes);
	emitSavedRegisters(emitter, 1);

	// THE SAME PROLOGUE TIER 1 EMITS, and it can be because the parameter slots
	// were laid out to match: slot 0 is the receiver, the arguments follow. That
	// is what lets the wide convention work here unchanged, and what keeps every
	// runtime helper's frame[-1] pointing at the running closure.
	uint16_t incoming = (uint16_t) (function->argumentCount + 1);
	if (!maWideForArity(abi, function->argumentCount)) {
		for (uint16_t i = 0; i < incoming; i++) {
			asmMovMemReg(buffer, X64_FRAME, slotOffset(i),
				(Register) abi->argumentRegisters[i]);
		}
	} else {
		Register block = (Register) abi->argumentRegisters[0];
		for (uint16_t i = 0; i < incoming; i++) {
			asmMovRegMem(buffer, X64_SCRATCH_A, block,
				-(int32_t) ((uint32_t) i * sizeof(Value)));
			asmMovMemReg(buffer, X64_FRAME, slotOffset(i), X64_SCRATCH_A);
		}
	}

	// Every remaining slot starts as nil, for the reason tier 1 does it: a slot
	// the frame map calls a pointer and nobody wrote would hand the collector
	// stack garbage as a root. Here that covers the outgoing argument area and
	// the spill slots, which are written later or not at all.
	if (function->frameSlots > incoming) {
		asmMovRegImm64(buffer, X64_SCRATCH_A, nilValue);
		for (uint16_t i = incoming; i < function->frameSlots; i++) {
			asmMovMemReg(buffer, X64_FRAME, slotOffset(i), X64_SCRATCH_A);
		}
	}
}


static void emitReturn(SsaEmitter *emitter)
{
	CodeBuffer *buffer = ssaEmitterBuffer(emitter);
	emitSavedRegisters(emitter, 0);
	asmMovRegReg(buffer, RSP, X64_FRAME);
	asmPop(buffer, X64_FRAME);
	asmRet(buffer);
}


// A binary integer operation, staged through the scratch so no combination of
// destination and operands can clobber an input before it is read.
static void emitBinary(SsaEmitter *emitter, const LirInstruction *it,
	void (*operation)(CodeBuffer *, Register, Register))
{
	CodeBuffer *buffer = ssaEmitterBuffer(emitter);
	asmMovRegReg(buffer, X64_SCRATCH_A, intReg(it->argReg[0]));
	operation(buffer, X64_SCRATCH_A, intReg(it->argReg[1]));
	asmMovRegReg(buffer, intReg(it->dstReg), X64_SCRATCH_A);
}


static void emitFloatBinary(SsaEmitter *emitter, const LirInstruction *it,
	uint8_t opcode)
{
	CodeBuffer *buffer = ssaEmitterBuffer(emitter);
	asmMovsdRegReg(buffer, X64_FSCRATCH, floatReg(it->argReg[0]));
	asmSseRegReg(buffer, 0xF2, opcode, X64_FSCRATCH, floatReg(it->argReg[1]));
	asmMovsdRegReg(buffer, floatReg(it->dstReg), X64_FSCRATCH);
}


static void x64SsaInstruction(SsaEmitter *emitter, const LirInstruction *it,
	SsaLabel *taken, SsaLabel *notTaken)
{
	CodeBuffer *buffer = ssaEmitterBuffer(emitter);
	const LirFunction *function = ssaEmitterFunction(emitter);
	const Abi *abi = ssaEmitterAbi(emitter);
	_Bool floating = it->dst != LIR_NO_VREG
		&& function->vregBank[it->dst] == LIR_BANK_FLOAT;

	switch ((LirOp) it->op) {
	case LIR_ENTRY:
		break;

	case LIR_MOVE:
		if (floating) {
			asmMovsdRegReg(buffer, floatReg(it->dstReg), floatReg(it->argReg[0]));
		} else {
			asmMovRegReg(buffer, intReg(it->dstReg), intReg(it->argReg[0]));
		}
		break;

	case LIR_IMM:
		asmMovRegImm64(buffer, intReg(it->dstReg), (uint64_t) it->imm);
		break;

	case LIR_LOAD:
		asmMovRegMem(buffer, intReg(it->dstReg), intReg(it->argReg[0]), it->disp);
		break;

	case LIR_STORE:
		asmMovMemReg(buffer, intReg(it->argReg[0]), it->disp,
			intReg(it->argReg[1]));
		break;

	case LIR_LOAD32:
		asmMov32RegMem(buffer, intReg(it->dstReg), intReg(it->argReg[0]), it->disp);
		break;

	case LIR_STORE32:
		asmMov32MemReg(buffer, intReg(it->argReg[0]), it->disp,
			intReg(it->argReg[1]));
		break;

	case LIR_LOAD_SLOT:
		if (floating) {
			asmMovsdRegMem(buffer, floatReg(it->dstReg), X64_FRAME,
				slotOffset((uint16_t) it->imm));
		} else {
			asmMovRegMem(buffer, intReg(it->dstReg), X64_FRAME,
				slotOffset((uint16_t) it->imm));
		}
		break;

	case LIR_STORE_SLOT:
		if (function->vregBank[it->args[0]] == LIR_BANK_FLOAT) {
			asmMovsdMemReg(buffer, X64_FRAME, slotOffset((uint16_t) it->imm),
				floatReg(it->argReg[0]));
		} else {
			asmMovMemReg(buffer, X64_FRAME, slotOffset((uint16_t) it->imm),
				intReg(it->argReg[0]));
		}
		break;

	case LIR_LOAD_ABS:
		// Through the destination, not the scratch: the address is a constant
		// and the destination is free, so no register has to be found.
		asmMovRegImm64(buffer, intReg(it->dstReg),
			(uint64_t) (uintptr_t) it->address);
		asmMovRegMem(buffer, intReg(it->dstReg), intReg(it->dstReg), 0);
		break;

	case LIR_FRAME_ADDR:
		asmLeaRegMem(buffer, intReg(it->dstReg), X64_FRAME,
			slotOffset((uint16_t) it->imm));
		break;

	case LIR_ADD: emitBinary(emitter, it, asmAddRegReg); break;
	case LIR_SUB: emitBinary(emitter, it, asmSubRegReg); break;
	case LIR_MUL: emitBinary(emitter, it, asmImulRegReg); break;
	case LIR_AND: emitBinary(emitter, it, asmAndRegReg); break;
	case LIR_OR:  emitBinary(emitter, it, asmOrRegReg); break;
	case LIR_XOR: emitBinary(emitter, it, asmXorRegReg); break;

	case LIR_DIV: case LIR_MOD:
		// idiv is hard-wired to RDX:RAX and destroys both, which is why
		// lirOpClobbers names these two: the allocator holds nothing live
		// across them. The divisor goes to the second scratch first, because it
		// could otherwise BE RDX and cqo would overwrite it.
		asmMovRegReg(buffer, X64_SCRATCH_B, intReg(it->argReg[1]));
		asmMovRegReg(buffer, X64_SCRATCH_A, intReg(it->argReg[0]));
		asmCqo(buffer);
		asmIdivReg(buffer, X64_SCRATCH_B);
		asmMovRegReg(buffer, intReg(it->dstReg),
			(LirOp) it->op == LIR_DIV ? RAX : RDX);
		break;

	case LIR_SHL: case LIR_SAR:
		// The count must be in CL, which is the second scratch.
		asmMovRegReg(buffer, X64_SCRATCH_B, intReg(it->argReg[1]));
		asmMovRegReg(buffer, X64_SCRATCH_A, intReg(it->argReg[0]));
		emitRexW(buffer, (Register) 0, X64_SCRATCH_A);
		emit8(buffer, 0xD3);
		emitModRmReg(buffer, (Register) ((LirOp) it->op == LIR_SHL ? 4 : 7),
			X64_SCRATCH_A);
		asmMovRegReg(buffer, intReg(it->dstReg), X64_SCRATCH_A);
		break;

	case LIR_ADDI: case LIR_ANDI: case LIR_SHLI: case LIR_SARI:
		asmMovRegReg(buffer, X64_SCRATCH_A, intReg(it->argReg[0]));
		switch ((LirOp) it->op) {
		case LIR_ADDI: asmAddRegImm32(buffer, X64_SCRATCH_A, (int32_t) it->imm); break;
		case LIR_ANDI: asmAnd32RegImm32(buffer, X64_SCRATCH_A, (uint32_t) it->imm); break;
		case LIR_SHLI: asmShlRegImm8(buffer, X64_SCRATCH_A, (uint8_t) it->imm); break;
		default:       asmSarRegImm8(buffer, X64_SCRATCH_A, (uint8_t) it->imm); break;
		}
		asmMovRegReg(buffer, intReg(it->dstReg), X64_SCRATCH_A);
		break;

	case LIR_NEG:
		asmMovRegReg(buffer, X64_SCRATCH_A, intReg(it->argReg[0]));
		asmNegReg(buffer, X64_SCRATCH_A);
		asmMovRegReg(buffer, intReg(it->dstReg), X64_SCRATCH_A);
		break;

	case LIR_FADD: emitFloatBinary(emitter, it, 0x58); break;
	case LIR_FSUB: emitFloatBinary(emitter, it, 0x5C); break;
	case LIR_FMUL: emitFloatBinary(emitter, it, 0x59); break;
	case LIR_FDIV: emitFloatBinary(emitter, it, 0x5E); break;

	case LIR_FNEG:
		// Through the INTEGER file, flipping the sign bit. There is no scalar
		// negate in SSE, and the usual trick (xorpd against a sign mask) needs
		// the mask in memory, which would mean a constant pool this backend does
		// not have yet.
		asmMovqGprFromXmm(buffer, X64_SCRATCH_A, floatReg(it->argReg[0]));
		asmMovRegImm64(buffer, X64_SCRATCH_B, (uint64_t) 1 << 63);
		asmXorRegReg(buffer, X64_SCRATCH_A, X64_SCRATCH_B);
		asmMovqXmmFromGpr(buffer, floatReg(it->dstReg), X64_SCRATCH_A);
		break;

	case LIR_FSQRT:
		asmSqrtsd(buffer, floatReg(it->dstReg), floatReg(it->argReg[0]));
		break;

	case LIR_I2F:
		asmCvtsi2sd(buffer, floatReg(it->dstReg), intReg(it->argReg[0]));
		break;

	case LIR_F2I:
		asmCvttsd2si(buffer, intReg(it->dstReg), floatReg(it->argReg[0]));
		break;

	case LIR_BITCAST_I2F:
		asmMovqXmmFromGpr(buffer, floatReg(it->dstReg), intReg(it->argReg[0]));
		break;

	case LIR_BITCAST_F2I:
		asmMovqGprFromXmm(buffer, intReg(it->dstReg), floatReg(it->argReg[0]));
		break;

	case LIR_JUMP:
		asmJmp(buffer, (X64Label *) taken);
		break;

	case LIR_CMP_BRANCH:
		asmCmpRegReg(buffer, intReg(it->argReg[0]), intReg(it->argReg[1]));
		asmJcc(buffer, conditionFor(it->condition), (X64Label *) taken);
		asmJmp(buffer, (X64Label *) notTaken);
		break;

	case LIR_CMP_BRANCH_IMM:
		// The immediate is a full 64-bit tagged value, so it goes through the
		// scratch rather than into a cmp's 32-bit field.
		asmMovRegImm64(buffer, X64_SCRATCH_A, (uint64_t) it->imm);
		asmCmpRegReg(buffer, intReg(it->argReg[0]), X64_SCRATCH_A);
		asmJcc(buffer, conditionFor(it->condition), (X64Label *) taken);
		asmJmp(buffer, (X64Label *) notTaken);
		break;

	case LIR_CMP_SET:
		if (it->argCount == 2
				&& function->vregBank[it->args[0]] == LIR_BANK_FLOAT) {
			asmUcomisd(buffer, floatReg(it->argReg[0]), floatReg(it->argReg[1]));
			asmSetcc(buffer, floatConditionFor(it->condition), X64_SCRATCH_A);
		} else {
			if (it->argCount == 2) {
				asmCmpRegReg(buffer, intReg(it->argReg[0]), intReg(it->argReg[1]));
			} else {
				asmMovRegImm64(buffer, X64_SCRATCH_B, (uint64_t) it->imm);
				asmCmpRegReg(buffer, intReg(it->argReg[0]), X64_SCRATCH_B);
			}
			asmSetcc(buffer, conditionFor(it->condition), X64_SCRATCH_A);
		}
		asmMovzxByte(buffer, intReg(it->dstReg), X64_SCRATCH_A);
		break;

	case LIR_RET:
		asmMovRegReg(buffer, (Register) abi->integerResult,
			intReg(it->argReg[0]));
		emitReturn(emitter);
		break;

	case LIR_CALL_RUNTIME3: {
		asmMovRegImm64(buffer, (Register) abi->argumentRegisters[0],
			(uint64_t) (uintptr_t) it->pointerArg);
		asmLeaRegMem(buffer, (Register) abi->argumentRegisters[1], X64_FRAME,
			slotOffset((uint16_t) it->disp));
		asmMovRegImm64(buffer, (Register) abi->argumentRegisters[2],
			(uint64_t) it->imm);
		if (abi->shadowSpaceBytes != 0) {
			asmSubRegImm32(buffer, RSP, abi->shadowSpaceBytes);
		}
		uint64_t target;
		memcpy(&target, &it->function, sizeof(target));
		asmMovRegImm64(buffer, X64_SCRATCH_A, target);
		asmCallReg(buffer, X64_SCRATCH_A);
		if (abi->shadowSpaceBytes != 0) {
			asmAddRegImm32(buffer, RSP, abi->shadowSpaceBytes);
		}
		if (it->dst != LIR_NO_VREG) {
			asmMovRegReg(buffer, intReg(it->dstReg),
				(Register) abi->integerResult);
		}
		break;
	}

	case LIR_GUARD_CLASS: {
		// A SPECULATION, and the whole sequence is here rather than in the
		// lowering because the branch structure between the check and the
		// failure is the backend's (ADR 0009).
		//
		// THE TAG IS CHECKED AGAINST VALUE_POINTER, not merely against
		// SmallInteger. An immediate has no header at all, so reading a class
		// index out of one reads whatever lies at address (value - 1); testing
		// only for tag 00 would let a Character and a SmallFloat64 through, and
		// those are exactly the receivers a test is least likely to try.
		X64Label *fail = (X64Label *) x64SsaNewLabel(emitter);
		X64Label *done = (X64Label *) x64SsaNewLabel(emitter);
		Register value = intReg(it->argReg[0]);
		asmMovRegReg(buffer, X64_SCRATCH_A, value);
		asmAnd32RegImm32(buffer, X64_SCRATCH_A, 3);
		asmCmp32RegImm32(buffer, X64_SCRATCH_A, (uint32_t) VALUE_POINTER);
		asmJcc(buffer, COND_NOT_EQUAL, fail);
		asmMovRegReg(buffer, X64_SCRATCH_A, value);
		asmSubRegImm32(buffer, X64_SCRATCH_A, (int32_t) VALUE_POINTER);
		asmMov32RegMem(buffer, X64_SCRATCH_B, X64_SCRATCH_A, 0);
		asmAnd32RegImm32(buffer, X64_SCRATCH_B, (uint32_t) OBJ_CLASS_MASK);
		asmCmp32RegImm32(buffer, X64_SCRATCH_B,
			(uint32_t) it->imm & (uint32_t) OBJ_CLASS_MASK);
		asmJcc(buffer, COND_EQUAL, done);

		asmBind(buffer, fail);
		// SPILL THE WHOLE REGISTER FILE first. A deopt state can name a value
		// living in any register, and by the time the C helper runs every
		// register belongs to the helper; the save area is what it reads
		// instead. Uniform rather than per-site, because the alternative is a
		// per-site list the emitter and the reader both have to agree about.
		for (uint8_t r = 0; r < DEOPT_SAVED_REGISTERS; r++) {
			asmMovMemReg(buffer, X64_FRAME,
				slotOffset((uint16_t) (function->deoptSaveBase + r)),
				(Register) r);
		}
		asmMovRegImm64(buffer, (Register) abi->argumentRegisters[0],
			(uint64_t) (uintptr_t) it->deoptSite);
		asmLeaRegMem(buffer, (Register) abi->argumentRegisters[1], X64_FRAME,
			slotOffset(0));
		asmMovRegImm64(buffer, (Register) abi->argumentRegisters[2], 0);
		if (abi->shadowSpaceBytes != 0) {
			asmSubRegImm32(buffer, RSP, abi->shadowSpaceBytes);
		}
		uint64_t helper;
		MaRuntimeFunction deoptimize = jitDeoptimize;
		memcpy(&helper, &deoptimize, sizeof(helper));
		asmMovRegImm64(buffer, X64_SCRATCH_A, helper);
		asmCallReg(buffer, X64_SCRATCH_A);
		if (abi->shadowSpaceBytes != 0) {
			asmAddRegImm32(buffer, RSP, abi->shadowSpaceBytes);
		}
		// The rest of the method ran in tier 1 and its answer is already in the
		// result register, so this method is finished. Falling through into the
		// tier-2 code below would run it a SECOND time.
		emitReturn(emitter);
		asmBind(buffer, done);
		break;
	}

	case LIR_SAFEPOINT: {
		X64Label *pass = (X64Label *) x64SsaNewLabel(emitter);
		asmMovRegImm64(buffer, X64_SCRATCH_A,
			(uint64_t) (uintptr_t) &CurrentThread.heap->safepointRequested);
		asmCmpMem32Imm32(buffer, X64_SCRATCH_A, 0, 0);
		asmJcc(buffer, COND_EQUAL, pass);
		// PENDING: park in heapGcPoll, exactly as tier 1's poll is pending.
		asmBind(buffer, pass);
		break;
	}

	default:
		// UNREACHABLE: the lowering refuses anything it cannot select, and every
		// op it can select is named above. Reaching here means the two lists
		// disagree, which is the silent-skip failure the refusal exists to
		// prevent, so it stops here and names the operation.
		fprintf(stderr, "ssa-x64: no emission for %s\n",
			lirOpName((LirOp) it->op));
		FAIL();
	}
}


const SsaEmitterOps gSsaEmitterX64SysV = {
	.name = "x64",
	.abi = &gAbiX64SysV,
	.stateBytes = sizeof(EmitterState),
	.begin = x64SsaBegin,
	.end = x64SsaEnd,
	.prologue = x64SsaPrologue,
	.newLabel = x64SsaNewLabel,
	.bind = x64SsaBind,
	.jump = x64SsaJump,
	.instruction = x64SsaInstruction,
};
