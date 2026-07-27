// Gate level 5: the optimizer.
//
// The centrepiece is the loop-carried accumulator, because it is the case that
// decides whether boxing elimination is worth anything at all.
//
// `total := total + x` is born as a TAGGED phi whose only producer is a box and
// whose only consumer is an unbox. Every iteration boxes a double and
// immediately unboxes it again. Local rewriting cannot see it: the box and the
// unbox are in different iterations, separated by a back edge, and neither is
// redundant on its own.
//
// Promoting the phi to F64 is what breaks it open, and afterwards the ordinary
// passes finish the job. Without step 7 the whole chain stops at the loop
// boundary, which is why the pass list has it and why this test exists.

#include "jit/Ir.h"
#include "jit/Passes.h"
#include "jit/SsaBuild.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int gFailures;
static int gChecks;

static void check(const char *what, int ok)
{
	gChecks++;
	if (!ok) {
		gFailures++;
		printf("  FAIL  %s\n", what);
	} else {
		printf("  ok    %s\n", what);
	}
}


static uint32_t countOp(IrFunction *function, IrOp op)
{
	uint32_t count = 0;
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->phis; value != NULL; value = value->next) {
			count += value->op == op;
		}
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			count += value->op == op;
		}
	}
	return count;
}


static IrValue *findOp(IrFunction *function, IrOp op)
{
	for (IrBlock *block = function->blocks; block != NULL; block = block->next) {
		for (IrValue *value = block->phis; value != NULL; value = value->next) {
			if (value->op == op) { return value; }
		}
		for (IrValue *value = block->first; value != NULL; value = value->next) {
			if (value->op == op) { return value; }
		}
	}
	return NULL;
}


static CodeUnit *emptyUnit(uint16_t registers)
{
	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	unit->registerCount = registers;
	unit->instructionCount = 0;
	return unit;
}


static IrValue *emitInto(IrFunction *function, IrBlock *block, IrOp op)
{
	IrValue *value = irNewValue(function, op);
	irAppend(block, value);
	return value;
}


static void link(IrFunction *function, IrBlock *from, IrBlock *to)
{
	from->succs[from->succCount++] = to;
	IrBlock **preds = irAlloc(function, (to->predCount + 1) * sizeof(IrBlock *));
	memcpy(preds, to->preds, to->predCount * sizeof(IrBlock *));
	preds[to->predCount] = from;
	to->preds = preds;
	to->predCount++;
}


int main(void)
{
	printf("gate level 5: the optimizer\n\n");

	// ---- the loop-carried accumulator --------------------------------------
	//
	//   B0: t0 = fconst 0.0 ; t1 = box_f t0            (total := 0.0)
	//   B1: t2 = phi(t1, t6)                           (loop-carried total)
	//       t3 = unbox_f t2
	//       t4 = fconst 1.5
	//       t5 = fadd t3, t4
	//       t6 = box_f t5
	//       branch back to B1 or on to B2
	//   B2: ret t2
	IrFunction *function = irCreate(emptyUnit(4));
	IrBlock *b0 = irNewBlock(function, 0);
	IrBlock *b1 = irNewBlock(function, 1);
	IrBlock *b2 = irNewBlock(function, 2);
	function->entry = b0;
	link(function, b0, b1);
	link(function, b1, b1);
	link(function, b1, b2);

	IrValue *zero = emitInto(function, b0, IR_FCONST);
	zero->fkonst = 0.0;
	IrValue *boxedZero = emitInto(function, b0, IR_BOX_F);
	irAddArg(function, boxedZero, zero);
	b0->terminator = irNewValue(function, IR_JUMP);
	b0->terminator->block = b0;

	IrValue *phi = irNewValue(function, IR_PHI);
	irAppend(b1, phi);
	IrValue *unboxed = emitInto(function, b1, IR_UNBOX_F);
	irAddArg(function, unboxed, phi);
	IrValue *step = emitInto(function, b1, IR_FCONST);
	step->fkonst = 1.5;
	IrValue *sum = emitInto(function, b1, IR_FADD);
	irAddArg(function, sum, unboxed);
	irAddArg(function, sum, step);
	IrValue *boxedSum = emitInto(function, b1, IR_BOX_F);
	irAddArg(function, boxedSum, sum);
	// Phi operands in predecessor order: the back edge first, then the entry.
	irAddArg(function, phi, boxedSum);
	irAddArg(function, phi, boxedZero);
	// The loop condition reads a SEPARATE value, not the accumulator. That is
	// the realistic shape (`0 to: n do:` tests the counter, not the total), and
	// it is what lets the loop body come out with no conversion at all.
	IrValue *flag = emitInto(function, b1, IR_PARAM);
	flag->extra = 1;
	IrValue *condition = emitInto(function, b1, IR_TAG2BOOL);
	irAddArg(function, condition, flag);
	b1->terminator = irNewValue(function, IR_BRANCH);
	irAddArg(function, b1->terminator, condition);
	b1->terminator->block = b1;

	IrValue *result = irNewValue(function, IR_RET);
	irAddArg(function, result, phi);
	result->block = b2;
	b2->terminator = result;

	check("before: the accumulator is a TAGGED phi", phi->repr == REPR_TAGGED);
	check("before: the loop boxes once per iteration",
		countOp(function, IR_BOX_F) == 2);
	check("before: and unboxes once per iteration",
		countOp(function, IR_UNBOX_F) == 1);

	PassStats stats = irOptimize(function);

	printf("\n  passes: phis triviais=%u  tipos=%u  guards=%u  simplificados=%u"
		"  escalarizados=%u  gvn=%u  hoisted=%u  phis promovidos=%u"
		"  mortos=%u  blocos=%u\n\n",
		stats.trivialPhis, stats.typesLearned, stats.guardsRemoved,
		stats.simplified, stats.scalarReplaced, stats.gvnRemoved, stats.hoisted,
		stats.phisPromoted, stats.deadRemoved, stats.blocksMerged);

	check("the loop-carried phi was promoted to a raw double",
		phi->repr == REPR_F64);
	check("promotion is reported", stats.phisPromoted == 1);
	check("the unbox on the loop-carried edge is gone",
		countOp(function, IR_UNBOX_F) == 0);
	// A box REMAINS, and it should: the method returns the accumulator, and a
	// return takes a tagged value. What matters is WHERE it is. It must not be
	// on the loop-carried edge, and it must not be inside the loop body.
	check("exactly one box survives, for the genuine tagged use",
		countOp(function, IR_BOX_F) == 1);
	uint32_t boxesInLoop = 0;
	for (IrValue *value = b1->first; value != NULL; value = value->next) {
		boxesInLoop += value->op == IR_BOX_F || value->op == IR_UNBOX_F;
	}
	check("the LOOP BODY has no conversion left at all", boxesInLoop == 0);
	IrValue *add = findOp(function, IR_FADD);
	check("the addition survived and now reads the phi directly",
		add != NULL && add->args[0] == phi);
	check("the phi's back-edge operand is the raw sum, not a box",
		phi->args[0] == add);

	printf("\n  the loop after optimization:\n");
	irPrint(function);
	irDestroy(function);

	// ---- redundant guards --------------------------------------------------
	// Two guards on the same value for the same class: the second is redundant
	// because the first already established it. The first must SURVIVE, which
	// is the trap the separation of `klass` and `known` exists to avoid.
	printf("\n");
	IrFunction *guards = irCreate(emptyUnit(2));
	IrBlock *g0 = irNewBlock(guards, 0);
	guards->entry = g0;
	IrValue *subject = emitInto(guards, g0, IR_PARAM);
	IrValue *first = emitInto(guards, g0, IR_GUARD_CLASS);
	irAddArg(guards, first, subject);
	first->extra = 7;
	IrValue *second = emitInto(guards, g0, IR_GUARD_CLASS);
	irAddArg(guards, second, subject);
	second->extra = 7;
	IrValue *third = emitInto(guards, g0, IR_GUARD_CLASS);
	irAddArg(guards, third, subject);
	third->extra = 9; // a DIFFERENT class: must not be removed
	g0->terminator = irNewValue(guards, IR_RET);
	irAddArg(guards, g0->terminator, subject);
	g0->terminator->block = g0;

	PassStats guardStats = irOptimize(guards);
	check("the redundant second guard is removed", guardStats.guardsRemoved == 1);
	check("the FIRST guard survives, having established the fact",
		countOp(guards, IR_GUARD_CLASS) == 2);
	_Bool firstSurvived = 0, differentSurvived = 0;
	for (IrValue *value = g0->first; value != NULL; value = value->next) {
		firstSurvived = firstSurvived || value == first;
		differentSurvived = differentSurvived || value == third;
	}
	check("specifically the first one, not merely two of them", firstSurvived);
	check("a guard for a different class is not touched", differentSurvived);
	irDestroy(guards);

	// ---- LICM: work that does not vary leaves the loop ---------------------
	//
	// A field read of a value defined outside the loop is performed once per
	// iteration until this pass moves it. Without that, "no tagged access
	// inside the loop" does not survive contact with reality.
	IrFunction *invariant = irCreate(emptyUnit(3));
	IrBlock *l0 = irNewBlock(invariant, 0);   // preheader
	IrBlock *l1 = irNewBlock(invariant, 1);   // loop header and body
	IrBlock *l2 = irNewBlock(invariant, 2);   // exit
	invariant->entry = l0;
	link(invariant, l0, l1);
	link(invariant, l1, l1);
	link(invariant, l1, l2);

	IrValue *hoistable = emitInto(invariant, l0, IR_PARAM);
	l0->terminator = irNewValue(invariant, IR_JUMP);
	l0->terminator->block = l0;

	// Read INSIDE the loop, of a value defined OUTSIDE it.
	IrValue *invariantRead = emitInto(invariant, l1, IR_FIELD_T);
	irAddArg(invariant, invariantRead, hoistable);
	invariantRead->extra = 2;
	// And a store, so the loop body is not entirely dead.
	IrValue *store = emitInto(invariant, l1, IR_SETFIELD_T);
	irAddArg(invariant, store, hoistable);
	irAddArg(invariant, store, invariantRead);
	store->extra = 3;
	IrValue *loopCondition = emitInto(invariant, l1, IR_TAG2BOOL);
	irAddArg(invariant, loopCondition, hoistable);
	l1->terminator = irNewValue(invariant, IR_BRANCH);
	irAddArg(invariant, l1->terminator, loopCondition);
	l1->terminator->block = l1;
	l2->terminator = irNewValue(invariant, IR_RET);
	irAddArg(invariant, l2->terminator, hoistable);
	l2->terminator->block = l2;

	check("before: the invariant read is inside the loop",
		invariantRead->block == l1);
	PassStats licmStats = irOptimize(invariant);
	check("LICM moved it to the preheader", invariantRead->block == l0);
	check("and reports what it moved", licmStats.hoisted >= 1);
	_Bool storeStayed = 0;
	for (IrValue *value = l1->first; value != NULL; value = value->next) {
		storeStayed = storeStayed || value == store;
	}
	check("the STORE stayed, because it is not pure", storeStayed);
	irDestroy(invariant);

	// A guard must NOT be hoisted: hoisting changes WHEN it fails, and its
	// deopt state names a bytecode index inside the loop, so resuming from the
	// preheader would resume at a point the program had not reached.
	IrFunction *guarded = irCreate(emptyUnit(3));
	IrBlock *m0 = irNewBlock(guarded, 0);
	IrBlock *m1 = irNewBlock(guarded, 1);
	IrBlock *m2 = irNewBlock(guarded, 2);
	guarded->entry = m0;
	link(guarded, m0, m1);
	link(guarded, m1, m1);
	link(guarded, m1, m2);
	IrValue *subjectValue = emitInto(guarded, m0, IR_PARAM);
	m0->terminator = irNewValue(guarded, IR_JUMP);
	m0->terminator->block = m0;
	IrValue *loopGuard = emitInto(guarded, m1, IR_GUARD_CLASS);
	irAddArg(guarded, loopGuard, subjectValue);
	loopGuard->extra = 5;
	IrValue *cond2 = emitInto(guarded, m1, IR_TAG2BOOL);
	irAddArg(guarded, cond2, subjectValue);
	m1->terminator = irNewValue(guarded, IR_BRANCH);
	irAddArg(guarded, m1->terminator, cond2);
	m1->terminator->block = m1;
	m2->terminator = irNewValue(guarded, IR_RET);
	irAddArg(guarded, m2->terminator, subjectValue);
	m2->terminator->block = m2;

	irOptimize(guarded);
	check("a guard is NOT hoisted out of a loop", loopGuard->block == m1);
	irDestroy(guarded);

	// ---- GVN over repeated pure work --------------------------------------
	IrFunction *repeated = irCreate(emptyUnit(2));
	IrBlock *r0 = irNewBlock(repeated, 0);
	repeated->entry = r0;
	IrValue *object = emitInto(repeated, r0, IR_PARAM);
	IrValue *loadA = emitInto(repeated, r0, IR_FIELD_T);
	irAddArg(repeated, loadA, object);
	loadA->extra = 3;
	IrValue *loadB = emitInto(repeated, r0, IR_FIELD_T);
	irAddArg(repeated, loadB, object);
	loadB->extra = 3;
	IrValue *use = emitInto(repeated, r0, IR_SEND);
	irAddArg(repeated, use, loadA);
	irAddArg(repeated, use, loadB);
	r0->terminator = irNewValue(repeated, IR_RET);
	irAddArg(repeated, r0->terminator, use);
	r0->terminator->block = r0;

	PassStats gvnStats = irOptimize(repeated);
	check("two identical field loads become one", gvnStats.gvnRemoved == 1);
	check("and both uses now name the survivor",
		use->args[0] == use->args[1] && use->args[0] == loadA);
	irDestroy(repeated);

	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
