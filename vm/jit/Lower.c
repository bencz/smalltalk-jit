// SSA IR -> LIR. Instruction selection, phi resolution and the frame layout.
//
// Three things happen here, and the order matters:
//
//   1. the CFG is copied, splitting critical edges, because a phi's copy has to
//      land on an EDGE and an edge with no block of its own has nowhere to put
//      one;
//   2. each instruction selects a short LIR sequence over virtual registers;
//   3. phis become parallel copies at the end of each predecessor.
//
// What this file does NOT do is decide where a value lives. Every operand is a
// virtual register and every temporary is a fresh one; the allocator is the only
// thing that has ever heard of a machine register (ADR 0009).

#include "jit/Lower.h"
#include "jit/Jit.h"
#include "jit/SsaRuntime.h"
#include "jit/Deopt.h"
#include "jit/InlineCache.h"
#include "core/Assert.h"
#include "core/Handle.h"
#include "runtime/Closure.h"
#include "runtime/Primitive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A tagged object's field i, biased by the pointer tag so the load needs no
// untagging at all. The same expression tier 1 emits (jit/x64/MacroAssemblerX64.c).
#define FIELD_DISP(i) ((int32_t) (HEADER_SIZE + (size_t) (i) * sizeof(Value)) - 1)

// Element k of a RawArray is field k+1: field 0 is the element count.
#define ARRAY_ELEMENT_FIELD(k) ((uint16_t) ((k) + 1))
// Field 1 of an Association is its value.
#define ASSOCIATION_VALUE_FIELD 1


typedef struct {
	IrFunction *ir;
	LirFunction *lir;
	NativeCode *tier1;
	// IR value id -> the vreg holding it. Sized by the IR's value counter, which
	// only ever grows, so an id is a dense index.
	uint32_t *vregOf;
	uint32_t vregOfCapacity;
	// IR block id -> its LIR block.
	LirBlock **blockOf;
	// The LIR block that owns edge (pred p of block b): either p's own block or
	// the block inserted to split a critical edge. Indexed [blockId][predIndex].
	LirBlock ***edgeBlock;
	LirBlock *current;
} Lowering;


// ---------------------------------------------------------------------------
// What can be lowered
// ---------------------------------------------------------------------------

// REFUSED BY NAME, never skipped. Three groups, and they are refused for three
// different reasons rather than one:
//
//   * the allocations and indexed accesses (IR_NEW, IR_ANEW, IR_ALOAD and
//     company): no producer creates them. The optimizer names them and the
//     front end emits no opcode that becomes one, so they are unreachable
//     today and would be untested code;
//   * IR_FIELD_F and IR_SETFIELD_F: raw fields belong to the value classes of
//     phase 7, which do not exist.
static _Bool canLower(IrOp op)
{
	switch (op) {
	case IR_CONST: case IR_LITERAL: case IR_PARAM: case IR_PHI:
	case IR_FIELD_T: case IR_SETFIELD_T:
	case IR_GLOBAL: case IR_SETGLOBAL:
	case IR_NEWCELL: case IR_CLOSURE:
	case IR_SEND: case IR_SAFEPOINT: case IR_GUARD_CLASS:
	case IR_RET: case IR_JUMP: case IR_BRANCH: case IR_RETOUTER:
	// The raw family. No producer creates these yet either -- nothing lowers a
	// send of `+` into an IR_FADD, because that needs the profile-driven
	// specialization that is still to come -- but they are the reason this
	// backend exists, so they are emitted and proved from hand-built IR, exactly
	// as gate levels 4 to 6 prove their mechanisms.
	case IR_FCONST: case IR_ICONST:
	case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
	case IR_FNEG: case IR_FSQRT:
	case IR_IADD: case IR_ISUB: case IR_IMUL: case IR_IDIV: case IR_IMOD:
	case IR_FCMP: case IR_ICMP:
	case IR_I2F: case IR_F2I:
	case IR_BOX_F: case IR_UNBOX_F: case IR_BOX_I: case IR_UNBOX_I:
	case IR_BOOL2TAG: case IR_TAG2BOOL:
		return 1;
	default:
		return 0;
	}
}


// The checked form of a raw operation, or LIR_OP_COUNT when it has none.
//
// THE ONE PLACE the correspondence is written. The up-front refusal and the
// instruction selection both read it, so "which operations can be checked" has
// a single answer; two copies of this list would let a producer set the flag on
// an operation the selection then lowered UNCHECKED, and a dropped overflow
// check is a silent wrong answer rather than a refusal.
static LirOp checkedFormOf(IrOp op)
{
	switch (op) {
	case IR_IADD: return LIR_GUARDED_ADD;
	case IR_ISUB: return LIR_GUARDED_SUB;
	case IR_IMUL: return LIR_GUARDED_MUL;
	default: return LIR_OP_COUNT;
	}
}


// ---------------------------------------------------------------------------
// Virtual registers
// ---------------------------------------------------------------------------

// What the collector must believe about a value of this representation.
//
// REPR_BOOL becomes SLOT_I64 and not a kind of its own: a boolean here is a 0
// or a 1 in a register, so it is a raw integer as far as scanning and as far as
// re-boxing on deoptimization are concerned. A separate kind would have to be
// understood by memory/Roots.h, which is the collector's contract, for a
// distinction the collector does not make.
static SlotKind kindOfRepr(Repr repr)
{
	switch (repr) {
	case REPR_F64: return SLOT_F64;
	case REPR_I64: case REPR_BOOL: return SLOT_I64;
	case REPR_TAGGED: return SLOT_POINTER;
	default: return SLOT_DEAD;
	}
}


static LirBank bankOfRepr(Repr repr)
{
	return repr == REPR_F64 ? LIR_BANK_FLOAT : LIR_BANK_INT;
}


static uint32_t vregFor(Lowering *lowering, IrValue *value)
{
	ASSERT(value->id < lowering->vregOfCapacity);
	if (lowering->vregOf[value->id] == LIR_NO_VREG) {
		Repr repr = (Repr) value->repr;
		lowering->vregOf[value->id] = lirNewVreg(lowering->lir, bankOfRepr(repr),
			kindOfRepr(repr));
	}
	return lowering->vregOf[value->id];
}


static uint32_t freshVreg(Lowering *lowering, LirBank bank, SlotKind kind)
{
	return lirNewVreg(lowering->lir, bank, kind);
}


static LirInstruction *emit(Lowering *lowering, LirOp op)
{
	return lirAppend(lowering->lir, lowering->current, op);
}


// ---------------------------------------------------------------------------
// The frame layout, decided before a single instruction is selected
// ---------------------------------------------------------------------------

// How many CONSECUTIVE frame slots a call's argument block needs. Reused by
// every call, so the area is sized by the largest.
static uint16_t outgoingSlotsFor(IrValue *value)
{
	switch ((IrOp) value->op) {
	case IR_SEND:
		return value->argCount;             // receiver plus arguments
	case IR_CLOSURE:
		// At least one even with no captures: jitMakeClosure is handed the base
		// slot's ADDRESS and turns it back into a frame pointer, so the slot has
		// to exist whether or not anything was written into it.
		return value->argCount > 0 ? value->argCount : 1;
	case IR_SETFIELD_T:
		return 2;                           // the object and the value
	case IR_SETGLOBAL: case IR_NEWCELL: case IR_RETOUTER:
	case IR_BOX_F: case IR_UNBOX_F: case IR_BOX_I: case IR_UNBOX_I:
		return 1;
	default:
		return 0;
	}
}


static void layoutFrame(Lowering *lowering)
{
	LirFunction *lir = lowering->lir;
	lir->argumentCount = lowering->ir->unit->argumentCount;
	lir->parameterSlots = (uint16_t) (lir->argumentCount + 1);
	lir->outgoingBase = lir->parameterSlots;

	uint16_t widest = 0;
	for (IrBlock *block = lowering->ir->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			uint16_t needed = outgoingSlotsFor(value);
			if (needed > widest) {
				widest = needed;
			}
		}
		if (block->terminator != NULL) {
			uint16_t needed = outgoingSlotsFor(block->terminator);
			if (needed > widest) {
				widest = needed;
			}
		}
	}
	lir->outgoingSlots = widest;
	// The register save area, reserved ONLY when the method can actually leave
	// speculatively. A method that cannot pays no slots and no instructions for
	// deoptimization, which is the same rule the non-local return already
	// follows.
	//
	// "CAN LEAVE" IS A PREDICATE AND NOT A LIST OF OPCODES, because it grew one:
	// checked arithmetic deoptimizes on overflow, and a layout that only looked
	// for IR_GUARD_CLASS would leave deoptSaveBase at zero while the emitter
	// spilled the register file over the receiver and the arguments.
	uint16_t afterOutgoing = (uint16_t) (lir->outgoingBase + lir->outgoingSlots);
	_Bool canDeoptimize = 0;
	for (IrBlock *block = lowering->ir->blocks; block != NULL && !canDeoptimize;
			block = block->next) {
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			if (irValueCanDeoptimize(value)) {
				canDeoptimize = 1;
				break;
			}
		}
	}
	if (canDeoptimize) {
		lir->deoptSaveBase = afterOutgoing;
		afterOutgoing = (uint16_t) (afterOutgoing + DEOPT_SAVE_SLOTS);
	}
	lir->spillBase = afterOutgoing;
	// The allocator raises this as it spills. It starts at the spill base rather
	// than at zero so that a method that spills nothing still reserves the
	// parameter and outgoing slots, which are written regardless.
	lir->frameSlots = lir->spillBase;
}


// Write one value into an outgoing slot. A store and not a move, because the
// runtime helpers read the frame and not the registers.
static void storeOutgoing(Lowering *lowering, uint16_t slot, uint32_t vreg)
{
	LirInstruction *store = emit(lowering, LIR_STORE_SLOT);
	store->args[0] = vreg;
	store->argCount = 1;
	store->imm = slot;
}


// The one call shape, with the outgoing block recorded on it. See LirInstruction
// in jit/Lir.h for why the block has to be recorded rather than merely written.
static LirInstruction *emitCall(Lowering *lowering, MaRuntimeFunction function,
	void *pointerArg, uint16_t slot, uint64_t integerArg, uint16_t outgoingCount,
	SlotKind outgoingKind)
{
	LirInstruction *call = emit(lowering, LIR_CALL_RUNTIME3);
	call->function = function;
	call->imm = (int64_t) integerArg;
	call->disp = (int32_t) slot;
	call->pointerArg = pointerArg;
	call->outgoingBase = slot;
	call->outgoingCount = outgoingCount;
	call->outgoingKind = (uint8_t) outgoingKind;
	return call;
}


// ---------------------------------------------------------------------------
// Instruction selection
// ---------------------------------------------------------------------------

// The tagged singleton an IR_CONST names. Baking the address is legal only
// because the three never move (allocateImmortalObject, ADR 0005), which
// jitCompileFor asserts on every compilation.
static Value constantValue(IrValue *value)
{
	switch ((Opcode) value->extra) {
	case OP_LOADNIL: return tagPtr(Handles.nil.raw);
	case OP_LOADTRUE: return tagPtr(Handles.true_.raw);
	case OP_LOADFALSE: return tagPtr(Handles.false_.raw);
	default: return value->konst;   // OP_LOADI, already tagged
	}
}


static LirCondition conditionOfIcmp(int64_t kind)
{
	switch ((IrCompare) kind) {
	case IR_CMP_EQ: return LIR_CMP_EQ;
	case IR_CMP_NE: return LIR_CMP_NE;
	case IR_CMP_LT: return LIR_CMP_LT;
	case IR_CMP_LE: return LIR_CMP_LE;
	case IR_CMP_GT: return LIR_CMP_GT;
	default: return LIR_CMP_GE;
	}
}


// The unit's literal frame, reached through the ADDRESS OF THE UNIT'S FIELD and
// never by baking the Array's address: the Array moves whenever the collector
// feels like it, the C struct holding the reference does not, and
// rootsVisitCompiledCode keeps that one word current. Identical to tier 1.
static uint32_t emitLiteralFrame(Lowering *lowering)
{
	uint32_t frame = freshVreg(lowering, LIR_BANK_INT, SLOT_POINTER);
	LirInstruction *load = emit(lowering, LIR_LOAD_ABS);
	load->dst = frame;
	load->address = &lowering->ir->unit->literals;
	return frame;
}


static uint32_t emitFieldLoad(Lowering *lowering, uint32_t object,
	uint16_t fieldIndex, SlotKind kind)
{
	uint32_t result = freshVreg(lowering, LIR_BANK_INT, kind);
	LirInstruction *load = emit(lowering, LIR_LOAD);
	load->dst = result;
	load->args[0] = object;
	load->argCount = 1;
	load->disp = FIELD_DISP(fieldIndex);
	return result;
}


static void lowerValue(Lowering *lowering, IrValue *value)
{
	LirFunction *lir = lowering->lir;
	uint16_t outgoing = lir->outgoingBase;

	switch ((IrOp) value->op) {
	case IR_PHI:
		// Nothing here. A phi is realized as copies at the end of each
		// predecessor, which is what resolvePhis does once every block exists.
		break;

	case IR_PARAM: {
		// A LOAD from the parameter's frame slot, which the prologue filled.
		// Modelling the incoming ABI registers instead would pin an interval to
		// a physical register before the allocator has run, for no gain: the
		// prologue has to write these slots anyway, because slot 0 is the
		// receiver every runtime helper reaches through.
		LirInstruction *load = emit(lowering, LIR_LOAD_SLOT);
		load->dst = vregFor(lowering, value);
		load->imm = value->extra;
		break;
	}

	case IR_CONST: {
		LirInstruction *immediate = emit(lowering, LIR_IMM);
		immediate->dst = vregFor(lowering, value);
		immediate->imm = (int64_t) constantValue(value);
		break;
	}

	case IR_ICONST: {
		LirInstruction *immediate = emit(lowering, LIR_IMM);
		immediate->dst = vregFor(lowering, value);
		immediate->imm = value->ikonst;
		break;
	}

	case IR_FCONST: {
		// The BIT PATTERN through the integer bank and then across, because no
		// target here has a 64-bit floating immediate.
		uint64_t bits;
		memcpy(&bits, &value->fkonst, sizeof(bits));
		uint32_t raw = freshVreg(lowering, LIR_BANK_INT, SLOT_I64);
		LirInstruction *immediate = emit(lowering, LIR_IMM);
		immediate->dst = raw;
		immediate->imm = (int64_t) bits;
		LirInstruction *cast = emit(lowering, LIR_BITCAST_I2F);
		cast->dst = vregFor(lowering, value);
		cast->args[0] = raw;
		cast->argCount = 1;
		break;
	}

	case IR_LITERAL: {
		uint32_t frame = emitLiteralFrame(lowering);
		uint32_t element = emitFieldLoad(lowering, frame,
			ARRAY_ELEMENT_FIELD(value->extra), SLOT_POINTER);
		LirInstruction *move = emit(lowering, LIR_MOVE);
		move->dst = vregFor(lowering, value);
		move->args[0] = element;
		move->argCount = 1;
		break;
	}

	case IR_GLOBAL: {
		uint32_t frame = emitLiteralFrame(lowering);
		uint32_t association = emitFieldLoad(lowering, frame,
			ARRAY_ELEMENT_FIELD(value->extra), SLOT_POINTER);
		uint32_t contents = emitFieldLoad(lowering, association,
			ASSOCIATION_VALUE_FIELD, SLOT_POINTER);
		LirInstruction *move = emit(lowering, LIR_MOVE);
		move->dst = vregFor(lowering, value);
		move->args[0] = contents;
		move->argCount = 1;
		break;
	}

	case IR_FIELD_T: {
		uint32_t object = vregFor(lowering, value->args[0]);
		uint32_t result = emitFieldLoad(lowering, object, (uint16_t) value->extra,
			SLOT_POINTER);
		LirInstruction *move = emit(lowering, LIR_MOVE);
		move->dst = vregFor(lowering, value);
		move->args[0] = result;
		move->argCount = 1;
		break;
	}

	case IR_SETFIELD_T:
		// THROUGH THE BARRIER, always. The IR cannot tell a cell store from an
		// instance-variable store -- both are IR_SETFIELD_T and CELL_VALUE_FIELD
		// is 0, which is also an ordinary instance-variable index -- and the only
		// safe reading of an indistinguishable pair is the conservative one. See
		// jitStoreField, which exists for this.
		storeOutgoing(lowering, outgoing, vregFor(lowering, value->args[0]));
		storeOutgoing(lowering, (uint16_t) (outgoing + 1),
			vregFor(lowering, value->args[1]));
		emitCall(lowering, jitStoreField, NULL, outgoing,
			(uint64_t) (outgoing + 1)
				| ((uint64_t) (uint16_t) value->extra << 16)
				| ((uint64_t) outgoing << 32),
			2, SLOT_POINTER);
		break;

	case IR_SETGLOBAL:
		storeOutgoing(lowering, outgoing, vregFor(lowering, value->args[0]));
		emitCall(lowering, jitStoreGlobal, lowering->ir->unit, outgoing,
			(uint64_t) value->extra, 1, SLOT_POINTER);
		break;

	case IR_NEWCELL: {
		storeOutgoing(lowering, outgoing, vregFor(lowering, value->args[0]));
		LirInstruction *call = emitCall(lowering, jitMakeCell, NULL,
			outgoing, (uint64_t) outgoing << 32, 1, SLOT_POINTER);
		call->dst = vregFor(lowering, value);
		break;
	}

	case IR_CLOSURE: {
		for (uint16_t i = 0; i < value->argCount; i++) {
			storeOutgoing(lowering, (uint16_t) (outgoing + i),
				vregFor(lowering, value->args[i]));
		}
		LirInstruction *call = emitCall(lowering, jitMakeClosure,
			lowering->ir->unit, outgoing,
			(uint64_t) (uint16_t) value->extra
				| ((uint64_t) value->argCount << 16)
				| ((uint64_t) outgoing << 32),
			value->argCount > 0 ? value->argCount : 1, SLOT_POINTER);
		call->dst = vregFor(lowering, value);
		break;
	}

	case IR_SEND: {
		// The receiver and its arguments into CONSECUTIVE slots, which is the
		// whole argument list as far as the runtime is concerned: consecutive
		// slots are at descending addresses, so one address describes the run.
		for (uint16_t i = 0; i < value->argCount; i++) {
			storeOutgoing(lowering, (uint16_t) (outgoing + i),
				vregFor(lowering, value->args[i]));
		}
		// THE SAME CELL TIER 1 USES, indexed by this send's own bytecode index.
		// Sharing keeps the profile cumulative across tiers and keeps a
		// deoptimization landing on caches that stayed warm.
		void *cell = lowering->tier1 != NULL
			? (void *) &lowering->tier1->cells[value->bci] : NULL;
		uint16_t argc = (uint16_t) (value->argCount - 1);
		LirInstruction *call = emitCall(lowering,
			(value->flags & IR_FLAG_SUPER) ? jitDispatchSuper
				: jitDispatch,
			cell, outgoing,
			(uint64_t) argc | ((uint64_t) outgoing << 32),
			value->argCount, SLOT_POINTER);
		call->dst = vregFor(lowering, value);
		// A send can leave optimized code, so it carries the state to resume
		// with. Attached HERE rather than reconstructed later, for the reason
		// SsaBuild attaches it at construction: a state built after the fact
		// describes a frame that no longer exists.
		call->deopt = value->deopt;
		break;
	}

	case IR_RETOUTER:
		storeOutgoing(lowering, outgoing, vregFor(lowering, value->args[0]));
		emitCall(lowering, jitReturnOuter, NULL, outgoing,
			(uint64_t) outgoing << 32, 1, SLOT_POINTER);
		break;

	case IR_SAFEPOINT: {
		LirInstruction *safepoint = emit(lowering, LIR_SAFEPOINT);
		safepoint->deopt = value->deopt;
		break;
	}

	case IR_GUARD_CLASS: {
		LirInstruction *guard = emit(lowering, LIR_GUARD_CLASS);
		guard->args[0] = vregFor(lowering, value->args[0]);
		guard->argCount = 1;
		guard->imm = value->extra;
		guard->deopt = value->deopt;
		break;
	}

	case IR_BOX_F: case IR_BOX_I: case IR_UNBOX_F: case IR_UNBOX_I: {
		// THROUGH A HELPER, and that is a stated first cut rather than the
		// design. A SmallFloat64 is a rotated exponent window with a heap box
		// for what does not fit (ADR 0005 and the numeric tower), so the inline
		// sequence is a compare, a rotate and a branch to an allocation; getting
		// that wrong is a wrong answer on subnormals and infinities, which is
		// exactly where a test is least likely to look. The call is correct
		// today and measurably slower, and the inline form is a later change
		// with a number attached.
		_Bool boxing = value->op == IR_BOX_F || value->op == IR_BOX_I;
		_Bool floating = value->op == IR_BOX_F || value->op == IR_UNBOX_F;
		uint32_t operand = vregFor(lowering, value->args[0]);
		if (boxing && floating) {
			// The raw double has to reach the frame as BYTES, so it crosses to
			// the integer bank first and the slot is described as SLOT_F64.
			uint32_t bits = freshVreg(lowering, LIR_BANK_INT, SLOT_I64);
			LirInstruction *cast = emit(lowering, LIR_BITCAST_F2I);
			cast->dst = bits;
			cast->args[0] = operand;
			cast->argCount = 1;
			operand = bits;
		}
		storeOutgoing(lowering, outgoing, operand);
		LirInstruction *call = emitCall(lowering,
			boxing ? (floating ? jitBoxFloat : jitBoxInteger)
				: (floating ? jitUnboxFloat : jitUnboxInteger),
			NULL, outgoing, (uint64_t) outgoing << 32, 1,
			boxing ? (floating ? SLOT_F64 : SLOT_I64) : SLOT_POINTER);
		if (!boxing && floating) {
			uint32_t bits = freshVreg(lowering, LIR_BANK_INT, SLOT_I64);
			call->dst = bits;
			LirInstruction *cast = emit(lowering, LIR_BITCAST_I2F);
			cast->dst = vregFor(lowering, value);
			cast->args[0] = bits;
			cast->argCount = 1;
		} else {
			call->dst = vregFor(lowering, value);
		}
		break;
	}

	case IR_BOOL2TAG: {
		// 0 or 1 becomes the false or the true SINGLETON. Arithmetic and not a
		// branch: the operand is known to be exactly 0 or 1 -- every producer of
		// a REPR_BOOL is a LIR_CMP_SET -- so
		//
		//     false + operand * (true - false)
		//
		// selects between them with no control flow and no conditional move,
		// which is what keeps it emittable on every target this LIR serves. The
		// two addresses are immortal (ADR 0005), so their difference is a
		// compile-time constant.
		uint32_t delta = freshVreg(lowering, LIR_BANK_INT, SLOT_I64);
		LirInstruction *span = emit(lowering, LIR_IMM);
		span->dst = delta;
		span->imm = (int64_t) tagPtr(Handles.true_.raw)
			- (int64_t) tagPtr(Handles.false_.raw);

		uint32_t scaled = freshVreg(lowering, LIR_BANK_INT, SLOT_I64);
		LirInstruction *multiply = emit(lowering, LIR_MUL);
		multiply->dst = scaled;
		multiply->args[0] = vregFor(lowering, value->args[0]);
		multiply->args[1] = delta;
		multiply->argCount = 2;

		uint32_t base = freshVreg(lowering, LIR_BANK_INT, SLOT_I64);
		LirInstruction *falseValue = emit(lowering, LIR_IMM);
		falseValue->dst = base;
		falseValue->imm = (int64_t) tagPtr(Handles.false_.raw);

		LirInstruction *add = emit(lowering, LIR_ADD);
		add->dst = vregFor(lowering, value);
		add->args[0] = scaled;
		add->args[1] = base;
		add->argCount = 2;
		break;
	}

	case IR_TAG2BOOL: {
		uint32_t result = vregFor(lowering, value);
		LirInstruction *set = emit(lowering, LIR_CMP_SET);
		set->dst = result;
		set->args[0] = vregFor(lowering, value->args[0]);
		set->argCount = 1;
		set->condition = LIR_CMP_EQ;
		set->imm = (int64_t) tagPtr(Handles.true_.raw);
		break;
	}

	case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
	case IR_IADD: case IR_ISUB: case IR_IMUL: case IR_IDIV: case IR_IMOD: {
		static const LirOp map[] = {
			[IR_FADD] = LIR_FADD, [IR_FSUB] = LIR_FSUB,
			[IR_FMUL] = LIR_FMUL, [IR_FDIV] = LIR_FDIV,
			[IR_IADD] = LIR_ADD, [IR_ISUB] = LIR_SUB,
			[IR_IMUL] = LIR_MUL, [IR_IDIV] = LIR_DIV, [IR_IMOD] = LIR_MOD,
		};
		// THE CHECKED FORM IS A DIFFERENT OPCODE, chosen here from the flag, so
		// that everything downstream -- the frame maps, the deopt sites, the
		// clobber question -- reads it off `op` like every other instruction
		// rather than re-deriving it from a flag the LIR does not carry.
		//
		// An operation carrying the flag with no checked form was already
		// refused by name up in lirLower, off the SAME function, so reaching the
		// selection means one exists.
		_Bool check = (value->flags & IR_FLAG_CHECK_OVERFLOW) != 0;
		LirInstruction *arithmetic = emit(lowering,
			check ? checkedFormOf((IrOp) value->op) : map[value->op]);
		arithmetic->dst = vregFor(lowering, value);
		arithmetic->args[0] = vregFor(lowering, value->args[0]);
		arithmetic->args[1] = vregFor(lowering, value->args[1]);
		arithmetic->argCount = 2;
		if (check) {
			arithmetic->deopt = value->deopt;
		}
		break;
	}

	case IR_FNEG: case IR_FSQRT: case IR_I2F: case IR_F2I: {
		static const LirOp map[] = {
			[IR_FNEG] = LIR_FNEG, [IR_FSQRT] = LIR_FSQRT,
			[IR_I2F] = LIR_I2F, [IR_F2I] = LIR_F2I,
		};
		LirInstruction *unary = emit(lowering, map[value->op]);
		unary->dst = vregFor(lowering, value);
		unary->args[0] = vregFor(lowering, value->args[0]);
		unary->argCount = 1;
		break;
	}

	case IR_FCMP: case IR_ICMP: {
		LirInstruction *compare = emit(lowering, LIR_CMP_SET);
		compare->dst = vregFor(lowering, value);
		compare->args[0] = vregFor(lowering, value->args[0]);
		compare->args[1] = vregFor(lowering, value->args[1]);
		compare->argCount = 2;
		compare->condition = (uint8_t) conditionOfIcmp(value->extra);
		break;
	}

	default:
		// UNREACHABLE: canLower refused everything this switch does not name,
		// before a single instruction was selected. Reaching here means the two
		// disagree, which is the silent-skip failure the refusal exists to
		// prevent, so it stops here loudly and by name.
		fprintf(stderr, "lower: %s passes canLower but is not selected\n",
			irOpName((IrOp) value->op));
		FAIL();
	}
}


static void lowerTerminator(Lowering *lowering, IrBlock *block)
{
	IrValue *terminator = block->terminator;
	if (terminator == NULL) {
		// A block with no terminator falls through, which the CFG already
		// records as a single successor.
		LirInstruction *jump = emit(lowering, LIR_JUMP);
		(void) jump;
		return;
	}

	switch ((IrOp) terminator->op) {
	case IR_RET: {
		LirInstruction *ret = emit(lowering, LIR_RET);
		ret->args[0] = vregFor(lowering, terminator->args[0]);
		ret->argCount = 1;
		break;
	}

	case IR_RETOUTER:
		// Never comes back, so no result is stored and nothing follows it. The
		// LIR_RET after it is unreachable and emitted anyway, because a block
		// without a terminator has no successor for the ordering to walk.
		lowerValue(lowering, terminator);
		emit(lowering, LIR_RET);
		break;

	case IR_JUMP:
		emit(lowering, LIR_JUMP);
		break;

	case IR_BRANCH: {
		// succs[0] is the branch TARGET and succs[1] the fall-through, which is
		// how buildCfg ordered them. `extra` says which sense: JUMPTRUE compares
		// against true, JUMPFALSE against false.
		LirInstruction *branch = emit(lowering, LIR_CMP_BRANCH_IMM);
		branch->args[0] = vregFor(lowering, terminator->args[0]);
		branch->argCount = 1;
		branch->condition = LIR_CMP_EQ;
		branch->imm = (int64_t) (terminator->extra
			? tagPtr(Handles.true_.raw) : tagPtr(Handles.false_.raw));
		break;
	}

	default:
		fprintf(stderr, "lower: %s is not a terminator this backend knows\n",
			irOpName((IrOp) terminator->op));
		FAIL();
	}
}


// ---------------------------------------------------------------------------
// Phis, as parallel copies on the edges
// ---------------------------------------------------------------------------

// Sequentialize a PARALLEL copy: every source is read before any destination is
// written. The order matters and the naive one is wrong -- `a := b; b := a`
// emitted in that order loses b -- so a destination that is still somebody's
// source waits, and a cycle is broken with a temporary.
//
// Two phis in a loop header that swap their values are exactly such a cycle, and
// they are not exotic: `a := b. b := a` inside a `whileTrue:` produces one.
typedef struct {
	uint32_t dst, src;
	_Bool done;
} ParallelCopy;


static void emitParallelCopy(Lowering *lowering, LirBlock *block,
	LirInstruction *before, ParallelCopy *copies, uint16_t count)
{
	LirBlock *saved = lowering->current;
	lowering->current = block;

	uint16_t remaining = count;
	while (remaining > 0) {
		_Bool progressed = 0;
		for (uint16_t i = 0; i < count; i++) {
			if (copies[i].done) {
				continue;
			}
			// Is this destination still needed as somebody else's source?
			_Bool blocked = 0;
			for (uint16_t j = 0; j < count; j++) {
				if (!copies[j].done && j != i && copies[j].src == copies[i].dst) {
					blocked = 1;
					break;
				}
			}
			if (blocked) {
				continue;
			}
			LirInstruction *move = lirInsertBefore(lowering->lir, block, before,
				LIR_MOVE);
			move->dst = copies[i].dst;
			move->args[0] = copies[i].src;
			move->argCount = 1;
			copies[i].done = 1;
			remaining--;
			progressed = 1;
		}
		if (progressed) {
			continue;
		}
		// Everything left is in a cycle. Break ONE of them by copying a source
		// aside; the rest of the loop then unblocks normally.
		for (uint16_t i = 0; i < count; i++) {
			if (copies[i].done) {
				continue;
			}
			uint32_t temporary = lirNewVreg(lowering->lir,
				(LirBank) lowering->lir->vregBank[copies[i].src],
				(SlotKind) lowering->lir->vregKind[copies[i].src]);
			LirInstruction *save = lirInsertBefore(lowering->lir, block, before,
				LIR_MOVE);
			save->dst = temporary;
			save->args[0] = copies[i].src;
			save->argCount = 1;
			for (uint16_t j = 0; j < count; j++) {
				if (!copies[j].done && copies[j].src == copies[i].src) {
					copies[j].src = temporary;
				}
			}
			break;
		}
	}
	lowering->current = saved;
}


static void resolvePhis(Lowering *lowering)
{
	for (IrBlock *block = lowering->ir->blocks; block != NULL; block = block->next) {
		uint16_t phiCount = 0;
		for (IrValue *phi = block->phis; phi != NULL; phi = phi->next) {
			phiCount++;
		}
		if (phiCount == 0) {
			continue;
		}
		ParallelCopy *copies = calloc(phiCount, sizeof(ParallelCopy));
		ASSERT(copies != NULL);

		for (uint16_t pred = 0; pred < block->predCount; pred++) {
			uint16_t count = 0;
			for (IrValue *phi = block->phis; phi != NULL; phi = phi->next) {
				// A phi whose operand count disagrees with the predecessor count
				// would silently read past the array. It cannot happen -- Braun
				// fills one operand per predecessor -- so it is checked rather
				// than defended against.
				ASSERT(pred < phi->argCount);
				uint32_t source = vregFor(lowering, phi->args[pred]);
				uint32_t destination = vregFor(lowering, phi);
				if (source == destination) {
					continue;   // the copy would be a no-op
				}
				copies[count].dst = destination;
				copies[count].src = source;
				copies[count].done = 0;
				count++;
			}
			if (count == 0) {
				continue;
			}
			LirBlock *edge = lowering->edgeBlock[block->id][pred];
			emitParallelCopy(lowering, edge, edge->last, copies, count);
		}
		free(copies);
	}
}


// ---------------------------------------------------------------------------
// The CFG, with critical edges split
// ---------------------------------------------------------------------------

// An edge is CRITICAL when its source has several successors and its target has
// several predecessors. A phi copy belongs on the edge, and a critical edge has
// no block of its own to put one in: placing it in the source would run it on
// the other successor's path too, and placing it in the target would run it for
// every predecessor.
static void buildCfg(Lowering *lowering)
{
	IrFunction *ir = lowering->ir;
	lowering->blockOf = calloc(ir->blockCount, sizeof(LirBlock *));
	lowering->edgeBlock = calloc(ir->blockCount, sizeof(LirBlock **));
	ASSERT(lowering->blockOf != NULL && lowering->edgeBlock != NULL);

	for (IrBlock *block = ir->blocks; block != NULL; block = block->next) {
		lowering->blockOf[block->id] = lirNewBlock(lowering->lir, block->label);
	}
	lowering->lir->entry = lowering->blockOf[ir->entry->id];

	for (IrBlock *block = ir->blocks; block != NULL; block = block->next) {
		lowering->edgeBlock[block->id] = calloc(block->predCount == 0 ? 1
			: block->predCount, sizeof(LirBlock *));
		ASSERT(lowering->edgeBlock[block->id] != NULL);
	}

	// The edges, in the IR's own successor order, so succs[0] stays the taken
	// arm of a branch all the way to the emitter.
	for (IrBlock *block = ir->blocks; block != NULL; block = block->next) {
		LirBlock *from = lowering->blockOf[block->id];
		for (uint8_t s = 0; s < block->succCount; s++) {
			IrBlock *target = block->succs[s];
			LirBlock *to = lowering->blockOf[target->id];
			// Which predecessor index is this block, in the target's list?
			uint16_t predIndex = 0;
			for (uint16_t p = 0; p < target->predCount; p++) {
				if (target->preds[p] == block) {
					predIndex = p;
					break;
				}
			}
			if (block->succCount > 1 && target->predCount > 1) {
				LirBlock *split = lirNewBlock(lowering->lir, target->label);
				lirAddEdge(lowering->lir, from, split);
				lirAddEdge(lowering->lir, split, to);
				lowering->edgeBlock[target->id][predIndex] = split;
			} else {
				lirAddEdge(lowering->lir, from, to);
				lowering->edgeBlock[target->id][predIndex] = from;
			}
		}
	}
}


// A split block holds the phi copies for one edge and then jumps. The jump is
// appended AFTER the copies, so it is emitted here once every copy is placed.
static void terminateSplitBlocks(Lowering *lowering)
{
	for (LirBlock *block = lowering->lir->blocks; block != NULL;
			block = block->next) {
		if (block->last == NULL || !lirOpIsTerminator((LirOp) block->last->op)) {
			lirAppend(lowering->lir, block, LIR_JUMP);
		}
	}
}


// ---------------------------------------------------------------------------

LirFunction *lirLower(IrFunction *ir, const Abi *abi, NativeCode *tier1,
	const char **refusedOp)
{
	for (IrBlock *block = ir->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->phis; value != NULL; value = value->next) {
			if (!canLower((IrOp) value->op)) {
				if (refusedOp != NULL) { *refusedOp = irOpName((IrOp) value->op); }
				return NULL;
			}
		}
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			if (!canLower((IrOp) value->op)) {
				if (refusedOp != NULL) { *refusedOp = irOpName((IrOp) value->op); }
				return NULL;
			}
			// A checked operation with no checked form, refused BY NAME like any
			// other thing this backend cannot select. Lowering it unchecked would
			// be the one failure this file must never produce: not a refusal, a
			// wrong answer with the overflow silently dropped.
			if ((value->flags & IR_FLAG_CHECK_OVERFLOW) != 0
					&& checkedFormOf((IrOp) value->op) == LIR_OP_COUNT) {
				if (refusedOp != NULL) { *refusedOp = irOpName((IrOp) value->op); }
				return NULL;
			}
		}
		if (block->terminator != NULL
				&& !canLower((IrOp) block->terminator->op)) {
			if (refusedOp != NULL) {
				*refusedOp = irOpName((IrOp) block->terminator->op);
			}
			return NULL;
		}
	}

	Lowering lowering;
	memset(&lowering, 0, sizeof(lowering));
	lowering.ir = ir;
	lowering.tier1 = tier1;
	lowering.lir = lirCreate(ir->unit, abi);
	lowering.lir->tier1 = tier1;
	lowering.vregOfCapacity = ir->valueCounter + 1;
	lowering.vregOf = malloc(lowering.vregOfCapacity * sizeof(uint32_t));
	ASSERT(lowering.vregOf != NULL);
	for (uint32_t i = 0; i < lowering.vregOfCapacity; i++) {
		lowering.vregOf[i] = LIR_NO_VREG;
	}

	layoutFrame(&lowering);
	buildCfg(&lowering);

	// THE PRIMITIVE ATTEMPT, and it goes in the PROLOGUE exactly as tier 1's
	// does, before a single bytecode has been selected.
	//
	// It is not an IR node, and that is the same decision tier 1 made: trying the
	// method's primitive is a property of the METHOD, not an instruction inside
	// it, and no pass would take a different decision because of it. LIR_CALL_
	// PRIMITIVE exists for it and had no producer until now.
	//
	// LEAVING IT OUT WAS NOT A MISSING OPTIMIZATION. `packages/Core` writes most
	// of the kernel as a pragma plus a Smalltalk fallback, and 65 of those
	// methods have NO fallback body at all, so the compiler gives them
	// `self primitiveFailed: #Name` (docs/jit-v2/01-gate.md). Tier-2 code that
	// skipped the attempt ran straight into that, and `primitiveFailed:` is
	// implemented by nobody: the first thing tier 2 did when it was finally
	// allowed to RUN was doesNotUnderstand on the receiver's own arithmetic.
	//
	// A DECLARED but unimplemented primitive emits nothing, which is the same
	// rule tier 1 follows and what lets the whole kernel compile while the
	// primitives arrive a few at a time.
	PrimitiveFunction primitive = ir->unit->primitive != PRIM_NONE
		? primitiveFunctionAt((PrimitiveNumber) ir->unit->primitive) : NULL;
	if (primitive != NULL) {
		lowering.current = lowering.lir->entry;
		LirInstruction *attempt = emit(&lowering, LIR_CALL_PRIMITIVE);
		attempt->primitive = primitive;
		attempt->imm = ir->unit->argumentCount;
		// THE RECEIVER AND ITS ARGUMENTS ARE THE ARGUMENT LIST, in the parameter
		// slots the prologue just wrote, so the collector has to be told they are
		// pointers at this call: a primitive allocates.
		attempt->outgoingBase = 0;
		attempt->outgoingCount = lowering.lir->parameterSlots;
		attempt->outgoingKind = SLOT_POINTER;
	}

	for (IrBlock *block = ir->blocks; block != NULL; block = block->next) {
		lowering.current = lowering.blockOf[block->id];
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			lowerValue(&lowering, value);
		}
		lowerTerminator(&lowering, block);
	}

	resolvePhis(&lowering);
	terminateSplitBlocks(&lowering);
	lirOrderAndNumber(lowering.lir);

	LirFunction *result = lowering.lir;
	// HANDED OVER, not freed: the allocator needs it to translate the deopt
	// states from values into locations, and it is the only thing that can.
	result->deoptVregOf = lowering.vregOf;
	result->deoptVregCapacity = lowering.vregOfCapacity;
	for (uint32_t i = 0; i < ir->blockCount; i++) {
		free(lowering.edgeBlock[i]);
	}
	free(lowering.edgeBlock);
	free(lowering.blockOf);
	return result;
}
