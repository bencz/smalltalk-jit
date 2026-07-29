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

	PassStats stats = irOptimize(function, NULL);

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

	PassStats guardStats = irOptimize(guards, NULL);
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
	PassStats licmStats = irOptimize(invariant, NULL);
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

	irOptimize(guarded, NULL);
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

	PassStats gvnStats = irOptimize(repeated, NULL);
	check("two identical field loads become one", gvnStats.gvnRemoved == 1);
	check("and both uses now name the survivor",
		use->args[0] == use->args[1] && use->args[0] == loadA);
	irDestroy(repeated);

	// ---- GVN must NOT carry a read across a store to the SAME field --------
	//
	// The defect this catches was live: IR_FIELD_T is pure, GVN numbered it, and
	// nothing invalidated the entry when a store went past. Two reads of one
	// field with an assignment between them collapsed into one, so the second
	// answered the value from before the assignment.
	IrFunction *stored = irCreate(emptyUnit(3));
	IrBlock *w0 = irNewBlock(stored, 0);
	stored->entry = w0;
	IrValue *target = emitInto(stored, w0, IR_PARAM);
	IrValue *fresh = emitInto(stored, w0, IR_PARAM);
	fresh->extra = 1;
	IrValue *readBefore = emitInto(stored, w0, IR_FIELD_T);
	irAddArg(stored, readBefore, target);
	readBefore->extra = 3;
	IrValue *overwrite = emitInto(stored, w0, IR_SETFIELD_T);
	irAddArg(stored, overwrite, target);
	irAddArg(stored, overwrite, fresh);
	overwrite->extra = 3;
	IrValue *readAfter = emitInto(stored, w0, IR_FIELD_T);
	irAddArg(stored, readAfter, target);
	readAfter->extra = 3;
	IrValue *pair = emitInto(stored, w0, IR_SEND);
	irAddArg(stored, pair, readBefore);
	irAddArg(stored, pair, readAfter);
	w0->terminator = irNewValue(stored, IR_RET);
	irAddArg(stored, w0->terminator, pair);
	w0->terminator->block = w0;

	irOptimize(stored, NULL);
	check("a field read is NOT reused across a store to that field",
		pair->args[0] != pair->args[1]);
	irDestroy(stored);

	// ---- but a store to a DIFFERENT field does not stop it -----------------
	//
	// The other half, and it is what keeps the fix from being "give up": the
	// field index is in the instruction, so a store to field 3 has no bearing on
	// a read of field 2.
	IrFunction *elsewhere = irCreate(emptyUnit(3));
	IrBlock *e0 = irNewBlock(elsewhere, 0);
	elsewhere->entry = e0;
	IrValue *other = emitInto(elsewhere, e0, IR_PARAM);
	IrValue *otherValue = emitInto(elsewhere, e0, IR_PARAM);
	otherValue->extra = 1;
	IrValue *readOne = emitInto(elsewhere, e0, IR_FIELD_T);
	irAddArg(elsewhere, readOne, other);
	readOne->extra = 2;
	IrValue *storeThree = emitInto(elsewhere, e0, IR_SETFIELD_T);
	irAddArg(elsewhere, storeThree, other);
	irAddArg(elsewhere, storeThree, otherValue);
	storeThree->extra = 3;
	IrValue *readTwo = emitInto(elsewhere, e0, IR_FIELD_T);
	irAddArg(elsewhere, readTwo, other);
	readTwo->extra = 2;
	IrValue *otherPair = emitInto(elsewhere, e0, IR_SEND);
	irAddArg(elsewhere, otherPair, readOne);
	irAddArg(elsewhere, otherPair, readTwo);
	e0->terminator = irNewValue(elsewhere, IR_RET);
	irAddArg(elsewhere, e0->terminator, otherPair);
	e0->terminator->block = e0;

	irOptimize(elsewhere, NULL);
	check("a read IS still reused across a store to a different field",
		otherPair->args[0] == otherPair->args[1]);
	irDestroy(elsewhere);

	// ---- GVN must NOT reuse a value that does not DOMINATE the use ---------
	//
	// Two arms of a conditional compute the same thing. Reverse post-order
	// visits one before the other, which the table mistook for availability:
	// neither arm reaches the other, so the replacement named a register that
	// was never written on the path actually taken.
	IrFunction *arms = irCreate(emptyUnit(2));
	IrBlock *a0 = irNewBlock(arms, 0);
	IrBlock *aLeft = irNewBlock(arms, 1);
	IrBlock *aRight = irNewBlock(arms, 2);
	arms->entry = a0;
	link(arms, a0, aLeft);
	link(arms, a0, aRight);
	IrValue *armSubject = emitInto(arms, a0, IR_PARAM);
	IrValue *pick = emitInto(arms, a0, IR_TAG2BOOL);
	irAddArg(arms, pick, armSubject);
	a0->terminator = irNewValue(arms, IR_BRANCH);
	irAddArg(arms, a0->terminator, pick);
	a0->terminator->block = a0;

	IrValue *leftRead = emitInto(arms, aLeft, IR_FIELD_T);
	irAddArg(arms, leftRead, armSubject);
	leftRead->extra = 4;
	aLeft->terminator = irNewValue(arms, IR_RET);
	irAddArg(arms, aLeft->terminator, leftRead);
	aLeft->terminator->block = aLeft;

	IrValue *rightRead = emitInto(arms, aRight, IR_FIELD_T);
	irAddArg(arms, rightRead, armSubject);
	rightRead->extra = 4;
	aRight->terminator = irNewValue(arms, IR_RET);
	irAddArg(arms, aRight->terminator, rightRead);
	aRight->terminator->block = aRight;

	irOptimize(arms, NULL);
	check("a value in a sibling block is NOT reused across it",
		countOp(arms, IR_FIELD_T) == 2);
	irDestroy(arms);

	// ---- LICM must NOT hoist a read out of a loop that writes THAT field ---
	IrFunction *written = irCreate(emptyUnit(3));
	IrBlock *n0 = irNewBlock(written, 0);
	IrBlock *n1 = irNewBlock(written, 1);
	IrBlock *n2 = irNewBlock(written, 2);
	written->entry = n0;
	link(written, n0, n1);
	link(written, n1, n1);
	link(written, n1, n2);
	IrValue *held = emitInto(written, n0, IR_PARAM);
	n0->terminator = irNewValue(written, IR_JUMP);
	n0->terminator->block = n0;
	IrValue *loopRead = emitInto(written, n1, IR_FIELD_T);
	irAddArg(written, loopRead, held);
	loopRead->extra = 7;
	IrValue *loopStore = emitInto(written, n1, IR_SETFIELD_T);
	irAddArg(written, loopStore, held);
	irAddArg(written, loopStore, loopRead);
	loopStore->extra = 7; // the SAME field the read reads
	IrValue *keepGoing = emitInto(written, n1, IR_TAG2BOOL);
	irAddArg(written, keepGoing, held);
	n1->terminator = irNewValue(written, IR_BRANCH);
	irAddArg(written, n1->terminator, keepGoing);
	n1->terminator->block = n1;
	n2->terminator = irNewValue(written, IR_RET);
	irAddArg(written, n2->terminator, held);
	n2->terminator->block = n2;

	irOptimize(written, NULL);
	check("a read of a field the loop WRITES stays in the loop",
		loopRead->block == n1);
	irDestroy(written);

	// ---- a field of a FRESH object, read across a store to it --------------
	//
	// `simplify` answers a field read from the allocation's own argument, which
	// is what makes scalar replacement possible and is wrong the moment anything
	// has written that field since. Same defect as the two above, third site.
	IrFunction *built = irCreate(emptyUnit(3));
	IrBlock *f0 = irNewBlock(built, 0);
	built->entry = f0;
	IrValue *atBirth = emitInto(built, f0, IR_PARAM);
	IrValue *later = emitInto(built, f0, IR_PARAM);
	later->extra = 1;
	IrValue *freshObject = emitInto(built, f0, IR_NEW);
	irAddArg(built, freshObject, atBirth);
	freshObject->extra = 9;
	IrValue *replaceField = emitInto(built, f0, IR_SETFIELD_T);
	irAddArg(built, replaceField, freshObject);
	irAddArg(built, replaceField, later);
	replaceField->extra = 0;
	IrValue *readBack = emitInto(built, f0, IR_FIELD_T);
	irAddArg(built, readBack, freshObject);
	readBack->extra = 0;
	f0->terminator = irNewValue(built, IR_RET);
	irAddArg(built, f0->terminator, readBack);
	f0->terminator->block = f0;

	irOptimize(built, NULL);
	check("a fresh object's field is NOT answered from the allocation "
		"across a store", f0->terminator->args[0] != atBirth);
	irDestroy(built);

	// ---- two cells holding the same value are two OBJECTS -----------------
	//
	// GVN excludes every allocation by name, and a cell is one. Collapsing them
	// would hand one box to two variables the program keeps apart, which is the
	// opposite of what a cell exists for.
	IrFunction *boxes = irCreate(emptyUnit(2));
	IrBlock *c0 = irNewBlock(boxes, 0);
	boxes->entry = c0;
	IrValue *heldValue = emitInto(boxes, c0, IR_PARAM);
	IrValue *cellA = emitInto(boxes, c0, IR_NEWCELL);
	irAddArg(boxes, cellA, heldValue);
	IrValue *cellB = emitInto(boxes, c0, IR_NEWCELL);
	irAddArg(boxes, cellB, heldValue);
	IrValue *usesBoth = emitInto(boxes, c0, IR_SEND);
	irAddArg(boxes, usesBoth, cellA);
	irAddArg(boxes, usesBoth, cellB);
	c0->terminator = irNewValue(boxes, IR_RET);
	irAddArg(boxes, c0->terminator, usesBoth);
	c0->terminator->block = c0;

	irOptimize(boxes, NULL);
	check("two cells holding the same value stay two cells",
		countOp(boxes, IR_NEWCELL) == 2);
	irDestroy(boxes);

	// ---- two DIFFERENT literals are not one value --------------------------
	//
	// The same disease as the two cells above, in the pass that numbers values
	// rather than the one that erases allocations, and it was real rather than
	// hypothetical: a literal load used to be an IR_CONST whose `extra` named
	// the OPCODE, so every literal load in a method agreed with every other on
	// op, extra, konst, argCount and repr -- which is the whole of gvnEqual. One
	// dominating the other was enough, and a straight line is enough for that.
	// The method then read one literal everywhere it should have read several.
	//
	// MEASURED before the split existed: two in, one out.
	static Instruction twoLiterals[] = {
		{ OP_LOADK, 0, 1, 0, 0 },
		{ OP_LOADK, 0, 2, 1, 0 },
		{ OP_MOVE,  0, 4, 2, 0 },   // an argument sits right above its receiver
		{ OP_MOVE,  0, 3, 1, 0 },
		{ OP_SEND,  1, 5, 0, 3 },   // both used, so neither dies for other reasons
		{ OP_RET,   0, 5, 0, 0 },
	};
	CodeUnit *literalUnit = calloc(1, sizeof(CodeUnit));
	literalUnit->code = twoLiterals;
	literalUnit->instructionCount = 6;
	literalUnit->registerCount = 8;
	IrFunction *literals = ssaBuild(literalUnit, NULL);
	irOptimize(literals, NULL);
	check("two different literals stay two values",
		countOp(literals, IR_LITERAL) == 2);
	irDestroy(literals);

	// ---- ARITHMETIC SPECIALIZATION FROM THE PROFILE -------------------------
	//
	// The pass that gives every pass above it something to do. Arithmetic
	// reaches the IR as an opaque send (ADR 0006), so until this existed, GVN,
	// LICM and phi promotion all ran over calls and moved nothing.
	//
	// Class indices here are ARBITRARY: this level links the IR and the passes
	// and nothing else, so there is no class table and no bootstrap. That is the
	// point of the table being data -- the pass never asks what class 7 is, it
	// only puts 7 in a guard.
	enum { CLASS_SMALLINT = 7, CLASS_SMALLFLOAT = 9 };

	// `^a + b`, with the site profiled as SmallInteger on both operands.
	static Instruction onePlus[] = {
		{ OP_MOVE, 0, 3, 0, 0 },   // r3 := self
		{ OP_MOVE, 0, 4, 1, 0 },   // r4 := arg, one register above its receiver
		{ OP_SEND, 1, 5, 0, 3 },   // r5 := r3 + r4
		{ OP_RET,  0, 5, 0, 0 },
	};
	CodeUnit *plusUnit = calloc(1, sizeof(CodeUnit));
	plusUnit->code = onePlus;
	plusUnit->instructionCount = 4;
	plusUnit->registerCount = 8;
	plusUnit->argumentCount = 1;

	SiteSpecialization *table = calloc(plusUnit->instructionCount,
		sizeof(SiteSpecialization));
	for (uint16_t i = 0; i < plusUnit->instructionCount; i++) {
		table[i].op = IR_OP_COUNT;
	}
	IrProfile profile = { table, plusUnit->instructionCount, CLASS_SMALLINT };
	table[2].op = IR_IADD;
	table[2].receiverClass = CLASS_SMALLINT;
	table[2].argumentClass = CLASS_SMALLINT;
	table[2].checkOverflow = 1;

	IrFunction *plus = ssaBuild(plusUnit, NULL);
	check("before: `a + b` is an opaque send", countOp(plus, IR_SEND) == 1);
	PassStats plusStats = irOptimize(plus, &profile);
	check("the send became raw arithmetic", countOp(plus, IR_SEND) == 0);
	check("specialization is reported", plusStats.sendsSpecialized == 1);
	check("and nothing was declined", plusStats.specializationsDeclined == 0);
	check("BOTH operands are guarded, not just the receiver",
		countOp(plus, IR_GUARD_CLASS) == 2);
	check("the addition is there", countOp(plus, IR_IADD) == 1);
	// THE CHECK IS ON THE ARITHMETIC, and this is the assertion that matters
	// most in this block: `+` on two SmallIntegers can leave the 62-bit payload
	// and the kernel's answer is a LargeInteger, which optimized code cannot
	// build. A specialization without it is a silent wrap at 64 bits.
	IrValue *addition = findOp(plus, IR_IADD);
	check("the addition is marked as overflow-checked",
		addition != NULL
			&& (addition->flags & IR_FLAG_CHECK_OVERFLOW) != 0);
	check("and it carries the state to leave with",
		addition != NULL && addition->deopt != NULL);
	check("the guards resume at the SEND, so tier 1 re-executes it",
		addition != NULL && addition->deopt->frames[0].bci == 2
			&& addition->deopt->frames[0].innermost);
	irDestroy(plus);

	// THE CONTROL, and it is the one that proves the pass is driven by the
	// table rather than by the opcode: the same method with no entry for the
	// site keeps its send. Without this, a pass that specialized every binary
	// send it saw would pass every check above.
	for (uint16_t i = 0; i < plusUnit->instructionCount; i++) {
		table[i].op = IR_OP_COUNT;
	}
	IrFunction *unprofiled = ssaBuild(plusUnit, NULL);
	PassStats coldStats = irOptimize(unprofiled, &profile);
	check("a site with no profile keeps its send",
		countOp(unprofiled, IR_SEND) == 1);
	check("and no guard is emitted for it",
		countOp(unprofiled, IR_GUARD_CLASS) == 0
			&& coldStats.sendsSpecialized == 0);
	irDestroy(unprofiled);
	free(table);

	// ---- IT COMPOSES, which is the entire claim -----------------------------
	//
	// `(a + b) + c`. Specialization alone would emit FOUR guards, two boxes and
	// two unboxes; what makes the result worth having is that the passes already
	// in this file then eat the middle. The intermediate box_i is known to
	// answer a SmallInteger, so the second site's guard on it is redundant and
	// goes; the unbox of that box collapses by the same local rule that collapses
	// any other. Three guards for three operands, one box for one answer.
	static Instruction chained[] = {
		{ OP_MOVE, 0, 4, 0, 0 },   // r4 := self
		{ OP_MOVE, 0, 5, 1, 0 },   // r5 := arg1
		{ OP_SEND, 1, 6, 0, 4 },   // r6 := r4 + r5
		{ OP_MOVE, 0, 7, 2, 0 },   // r7 := arg2, above the next receiver
		{ OP_MOVE, 0, 6, 6, 0 },
		{ OP_SEND, 1, 8, 0, 6 },   // r8 := r6 + r7
		{ OP_RET,  0, 8, 0, 0 },
	};
	CodeUnit *chainUnit = calloc(1, sizeof(CodeUnit));
	chainUnit->code = chained;
	chainUnit->instructionCount = 7;
	chainUnit->registerCount = 12;
	chainUnit->argumentCount = 2;

	SiteSpecialization *chainTable = calloc(chainUnit->instructionCount,
		sizeof(SiteSpecialization));
	for (uint16_t i = 0; i < chainUnit->instructionCount; i++) {
		chainTable[i].op = IR_OP_COUNT;
	}
	for (uint16_t bci = 2; bci <= 5; bci += 3) {
		chainTable[bci].op = IR_IADD;
		chainTable[bci].receiverClass = CLASS_SMALLINT;
		chainTable[bci].argumentClass = CLASS_SMALLINT;
		chainTable[bci].checkOverflow = 1;
	}

	IrFunction *chain = ssaBuild(chainUnit, NULL);
	IrProfile chainProfile = { chainTable, chainUnit->instructionCount,
		CLASS_SMALLINT };
	PassStats chainStats = irOptimize(chain, &chainProfile);
	check("two chained adds are both specialized",
		chainStats.sendsSpecialized == 2 && countOp(chain, IR_SEND) == 0);
	check("three operands, THREE guards and not four",
		countOp(chain, IR_GUARD_CLASS) == 3);
	// ONE unbox per OPERAND and one box for the one answer. Three unboxes is
	// the right number and two would be wrong: a + b + c reads three tagged
	// values. What had to disappear is the pair in the MIDDLE, where the first
	// addition's answer was boxed only to be unboxed again by the second.
	check("the intermediate box and unbox are gone: one box for one answer",
		countOp(chain, IR_BOX_I) == 1 && countOp(chain, IR_UNBOX_I) == 3
			&& chainStats.boxesSunk == 1);
	irDestroy(chain);
	free(chainTable);

	// ---- AND IT REACHES THE LOOP, which is what the whole tier is for -------
	//
	//   sum := 0. [ ... ] whileTrue: [ sum := sum + step ]
	//
	// Specialization makes the accumulator a box feeding a phi feeding an unbox,
	// which is exactly the shape phi promotion exists to break. After it, the
	// loop body holds a raw add and NO conversion at all: the accumulator lives
	// in a register across the back edge. That is the level-16 target in
	// miniature, reached by the passes that were already here.
	static Instruction accumulate[] = {
		{ OP_LOADI,    0, 3, 0, 0 },   // 0: sum := 0
		{ OP_LOADI,    0, 4, 1, 0 },   // 1: step := 1
		{ OP_SAFEPOINT,0, 0, 0, 0 },   // 2: loop head
		{ OP_SEND,     1, 3, 0, 3 },   // 3: sum := sum + step
		{ OP_JUMPTRUE, 0, 1, 6, 0 },   // 4: if arg -> 6
		{ OP_JUMP,     0, 2, 0, 0 },   // 5: -> 2
		{ OP_RET,      0, 3, 0, 0 },   // 6
	};
	CodeUnit *loopUnit = calloc(1, sizeof(CodeUnit));
	loopUnit->code = accumulate;
	loopUnit->instructionCount = 7;
	loopUnit->registerCount = 8;
	loopUnit->argumentCount = 1;

	SiteSpecialization *loopTable = calloc(loopUnit->instructionCount,
		sizeof(SiteSpecialization));
	for (uint16_t i = 0; i < loopUnit->instructionCount; i++) {
		loopTable[i].op = IR_OP_COUNT;
	}
	loopTable[3].op = IR_IADD;
	loopTable[3].receiverClass = CLASS_SMALLINT;
	loopTable[3].argumentClass = CLASS_SMALLINT;
	loopTable[3].checkOverflow = 1;

	IrFunction *accumulator = ssaBuild(loopUnit, NULL);
	IrProfile loopProfile = { loopTable, loopUnit->instructionCount,
		CLASS_SMALLINT };
	PassStats loopStats = irOptimize(accumulator, &loopProfile);
	check("the accumulator's send became an addition",
		loopStats.sendsSpecialized == 1 && countOp(accumulator, IR_IADD) == 1);
	check("the loop-carried phi was promoted to a raw integer",
		loopStats.phisPromoted >= 1);
	// The pair of numbers, not one of them: ONE box survives, on the way out to
	// the return, and ZERO unboxes remain because there is nothing left to
	// unbox. One number alone would not distinguish this from a loop the
	// optimizer simply deleted.
	printf("\n  the specialized loop after optimization:\n");
	irPrint(accumulator);
	check("the loop body carries no conversion at all",
		countOp(accumulator, IR_UNBOX_I) == 0);
	check("and exactly one box remains, for the answer",
		countOp(accumulator, IR_BOX_I) == 1);
	irDestroy(accumulator);
	free(loopTable);

	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
