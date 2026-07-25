#ifndef BOOLEAN_BRANCH_FUSE_H
#define BOOLEAN_BRANCH_FUSE_H

// Recognising the PAIR of JUMP_NOT_MEMBER_OF that every inlined conditional
// emits, so a backend can answer both halves with one test. Shared by both
// backends for the same reason SendClassify.h is: the two must agree exactly
// about which pairs are fusable, because the difference between "fusable" and
// "not" is whether a check gets skipped.
//
// compileBooleanKind (compiler/Compiler.c) emits an inlined ifTrue:ifFalse:,
// and:, or: and whileTrue: as:
//
//     JUMP_NOT_MEMBER_OF True, X -> lFalse
//       <true arm>
//       JUMP lEnd
//     lFalse:
//     JUMP_NOT_MEMBER_OF False, X -> lNotBool
//       <false arm>
//       ...
//     lNotBool:
//       SEND #mustBeBoolean X
//
// The two checks are not adjacent -- the true arm sits between them -- so this
// is not a peephole over consecutive bytecodes. What makes the fusion possible
// is that the second check IS the first one's jump target: at the moment the
// backend emits the first check it can read the second, and emit a single
// three-way branch (true -> fall through, false -> lFalse, neither -> lNotBool)
// that answers both. The second check then compiles to NOTHING, and because it
// compiles to nothing, jumping to its offset still lands exactly on the false
// arm -- the first check's target label needs no adjustment at all.
//
// Measured on Richards before the change: the second check re-loads the
// receiver AND the instance variable and re-runs the singleton compares, so the
// false path through `^taskHolding or: [...]` spent 14 instructions where 9 do.
//
// SAFETY, all five conditions required. The caller owns 2 and 3 because only it
// has the referrer counts and the speculation-site map.
//  1. The bytecode at the target really is JUMP_NOT_MEMBER_OF on the
//     COMPLEMENTARY boolean class over the SAME operand (checked here).
//  2. Nothing else jumps to that offset and it is not a loop header. The second
//     check is skipped, so any other path reaching it would skip a check it
//     needs. compileBooleanKind emits exactly one referrer, but the backend
//     must not assume the compiler is the only producer: the tier-1 inliner
//     (compiler/Optimizer.c) rewrites these bytecodes too.
//  3. Neither check is a speculation guard (jit/SpecSite.h). A guard has to end
//     in one fixed-width branch for targetPoisonGuardBranch to rewrite.
//  4. The operand is a DYNAMIC kind (checked here). A constant operand is
//     already folded away by each check separately, which beats fusing.
//  5. The first check's jump is FORWARD (checked here). A backward one is a
//     loop back-edge, whose target the backend has already emitted.

#include "compiler/Bytecodes.h"
#include "core/CompiledCode.h"

// Same variable, textually? Fusing on two operands that merely hold equal
// values would be wrong: the second check must be the same query as the first.
static _Bool boolFuseSameOperand(Operand a, Operand b)
{
	if (a.type != b.type) {
		return 0;
	}
	switch (a.type) {
	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:
	case OPERAND_INST_VAR:
	case OPERAND_ASSOC:
		return a.index == b.index;
	case OPERAND_CONTEXT_VAR:
		return a.index == b.index && a.level == b.level;
	case OPERAND_INST_VAR_OF:
		return a.index == b.index
			&& a.instance.type == b.instance.type
			&& a.instance.index == b.instance.index
			&& a.instance.level == b.instance.level;
	default:
		return 0; // constants and super: condition 4
	}
}


// The operand kinds that reach generateClassCheckOnReg, i.e. the ones whose
// check is real work rather than a compile-time decision.
static _Bool boolFuseOperandIsDynamic(Operand operand)
{
	switch (operand.type) {
	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:
	case OPERAND_INST_VAR:
	case OPERAND_INST_VAR_OF:
	case OPERAND_CONTEXT_VAR:
	case OPERAND_ASSOC:
		return 1;
	default:
		return 0;
	}
}


// Conditions 1, 4 and 5. `target` MUST already be known to be a bytecode
// boundary (the backend's prepass records that), so this decodes in place
// rather than re-walking the stream: a linear scan per attempt would make
// codegen quadratic in bytecode size, and the 64KB-per-method ceiling is real.
// On success, *outNotBoolTarget is where the second check sends a receiver that
// is neither true nor false, and *outSecondNumber is its bytecode number (which
// the caller needs for the speculation-site map).
static _Bool boolFuseSecondCheckAt(CompiledCode *code, ptrdiff_t target, _Bool forward,
	RawObject *firstClass, Operand operand,
	ptrdiff_t *outNotBoolTarget, ptrdiff_t *outSecondNumber, ptrdiff_t secondNumberHint)
{
	RawObject *trueClass = (RawObject *) Handles.True->raw;
	RawObject *falseClass = (RawObject *) Handles.False->raw;

	if (!forward) {
		return 0;
	}
	if (firstClass != trueClass && firstClass != falseClass) {
		return 0;
	}
	if (!boolFuseOperandIsDynamic(operand)) {
		return 0;
	}
	if (target < 0 || target >= (ptrdiff_t) code->bytecodesSize) {
		return 0;
	}

	BytecodesIterator it;
	bytecodeInitIterator(&it, code->bytecodes + target, code->bytecodesSize - target);
	if (bytecodeNext(&it) != BYTECODE_JUMP_NOT_MEMBER_OF) {
		return 0;
	}
	RawObject *secondClass = compiledCodeLiteralAt(code, bytecodeNextUint16(&it));
	Operand secondOperand = bytecodeNextOperand(&it);
	int32_t disp = bytecodeNextInt32(&it);

	if (secondClass != (firstClass == trueClass ? falseClass : trueClass)
			|| !boolFuseSameOperand(operand, secondOperand)) {
		return 0;
	}
	// bytecodeOffset is relative to the sub-stream, so add the target back.
	*outNotBoolTarget = target + bytecodeOffset(&it) + disp;
	*outSecondNumber = secondNumberHint;
	return 1;
}

#endif
