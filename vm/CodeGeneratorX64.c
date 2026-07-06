#include <stdio.h>
#include <stdlib.h>
#include "CodeGenerator.h"
#include "AssemblerX64.h"
#include "Object.h"
#include "Class.h"
#include "Smalltalk.h"
#include "Primitives.h"
#include "StubCode.h"
#include "Heap.h"
#include "StackFrame.h"
#include "Handle.h"
#include "Bytecodes.h"
#include "Compiler.h"
#include "CodeDescriptors.h"
#include "Thread.h"
#include "Assert.h"
#include <string.h>

typedef struct {
	ptrdiff_t offset;
	AssemblerLabel label;
} BytecodeLabel;

static NativeCode *generateBlockCode(CompiledBlock *block, CodeGenerator *parentGenerator);
static void generateCode(CodeGenerator *generator);
static void initCodeGenerator(CodeGenerator *generator);
static void freeCodeGenerator(CodeGenerator *generator);
static void generatePrologue(CodeGenerator *generator, size_t frameSize);
static void generateEpilogue(CodeGenerator *generator);
static void generateContextDefinition(CodeGenerator *generator);
static void generateContextRestore(CodeGenerator *generator);
static void generateBody(CodeGenerator *generator);
static void generateCopy(CodeGenerator *generator, BytecodesIterator *iterator);
static void generateSend(CodeGenerator *generator, BytecodesIterator *iterator);
static void generateOuterReturn(CodeGenerator *generator, BytecodesIterator *iterator);
static void pushOperand(CodeGenerator *generator, Operand operand);
static void movOperand(CodeGenerator *generator, Operand operand, Register reg);
static void movToOperand(CodeGenerator *generator, Register reg, Operand operand);
static void generateClassCheck(CodeGenerator *generator, Operand operand, RawClass *class, AssemblerLabel *label);
static void generateLoadBlock(CodeGenerator *generator, Operand operand);
static void fillContext(CodeGenerator *generator, uint8_t level);
static void fillAssoc(CodeGenerator *generator, uint8_t index);
static void fillVar(CodeGenerator *generator, Variable *var);
static void spillVar(CodeGenerator *generator, Variable *var);
static void movVar(CodeGenerator *generator, Variable *var, Register reg);
static void movToVar(CodeGenerator *generator, Register reg, Variable *var);
static Variable *variableAt(CodeGenerator *generator, ptrdiff_t index);
static Variable *specialVariableAt(CodeGenerator *generator, uint8_t type, ptrdiff_t index);


NativeCode *generateMethodCode(CompiledMethod *method)
{
	HandleScope scope;
	openHandleScope(&scope);

	CodeGenerator generator;
	initMethodCompiledCode(&generator.code, method);
	initCodeGenerator(&generator);
	generateCode(&generator);

	NativeCode *code = buildNativeCode(&generator);
	closeHandleScope(&scope, NULL);
	freeCodeGenerator(&generator);
	return code;
}


static NativeCode *generateBlockCode(CompiledBlock *block, CodeGenerator *parentGenerator)
{
	CodeGenerator generator;
	initBlockCompiledCode(&generator.code, block);
	initCodeGenerator(&generator);
	generateCode(&generator);

	NativeCode *code = buildNativeCode(&generator);
	compiledBlockSetNativeCode(block, code);
	freeCodeGenerator(&generator);
	return code;
}


static void generateCode(CodeGenerator *generator)
{
#ifdef PROFILE_METHOD_USAGE
	// Per-method-entry invocation counter, read only by printMethodsUsage (debug).
	// Off by default: it was a memory RMW on every single call/primitive.
	asmIncqMem(&generator->buffer, asmMem(RIP, NO_REGISTER, SS_1, -(sizeof(size_t) + 7)));
#endif

	if (generator->code.header.primitive > 0) {
		generator->regsAlloc.varsSize = 1;
		generator->regsAlloc.vars[0].flags |= VAR_DEFINED | VAR_ON_STACK;
		generator->regsAlloc.vars[0].frameOffset = -2;
		generator->regsAlloc.frameSize = generator->frameSize = 2;
		generatePrimitive(generator, generator->code.header.primitive);
	}
	computeRegsAlloc(&generator->regsAlloc, &X64AvailableRegs, &generator->code);
	if (!generator->regsAlloc.frameLess) {
		generatePrologue(generator, generator->regsAlloc.frameSize);
		generateContextDefinition(generator);
	} else if (generator->code.isBlock) {
		variableAt(generator, CONTEXT_INDEX)->flags |= VAR_IN_REG;
	}
	generateBody(generator);
	if (!generator->regsAlloc.frameLess) {
		generateContextRestore(generator);
		generateEpilogue(generator);
	} else {
		asmRet(&generator->buffer);
	}
}


static void initCodeGenerator(CodeGenerator *generator)
{
	asmInitBuffer(&generator->buffer, 256);
	generator->frameRawAreaSize = 0;
	generator->tmpVar = 0;
	generator->bytecodeNumber = 0;
	generator->stackmaps = newOrdColl(32);
	generator->descriptors = newOrdColl(32);
}


static void freeCodeGenerator(CodeGenerator *generator)
{
	asmFreeBuffer(&generator->buffer);
}


static void generatePrologue(CodeGenerator *generator, size_t frameSize)
{
	generator->frameSize = frameSize;
	asmPushq(&generator->buffer, RBP);
	asmMovq(&generator->buffer, RSP, RBP);
	asmSubqImm(&generator->buffer, RSP, generator->frameSize * sizeof(intptr_t));
	// Nil-initialise the local frame slots. A temp assigned on only one arm of an
	// inlined conditional (or otherwise not written on every path) would otherwise
	// read as stack garbage — wrong for Smalltalk semantics, and worse, the GC
	// stackmap can mark such a slot as a live root and the scavenger then chases the
	// garbage. The context/return-IC slots are overwritten right after this by
	// generateContextDefinition.
	if (frameSize > 0) {
		generateLoadObject(&generator->buffer, Handles.nil->raw, TMP, 1);
		for (size_t i = 0; i < frameSize; i++) {
			asmMovqToMem(&generator->buffer, TMP, asmMem(RBP, NO_REGISTER, SS_1, -(ptrdiff_t)(i + 1) * sizeof(intptr_t)));
		}
	}
}


static void generateEpilogue(CodeGenerator *generator)
{
	asmAddqImm(&generator->buffer, RSP, generator->frameSize * sizeof(intptr_t));
	asmPopq(&generator->buffer, RBP);
	asmRet(&generator->buffer);
}


static void generateContextDefinition(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	Variable *context = variableAt(generator, CONTEXT_INDEX);
	context->flags |= VAR_IN_REG;

	// spill native code entry
	asmMovqToMem(buffer, R11, asmMem(RBP, NO_REGISTER, SS_1, -sizeof(intptr_t)));

	if (generator->code.isBlock) {
		// setup RBP
		asmMovqToMem(buffer, RBP, asmMem(context->reg, NO_REGISTER, SS_1, varOffset(RawContext, frame)));
		spillVar(generator, context);

	} else {
		if (generator->code.header.hasContext) {
			generateMethodContextAllocation(generator, generator->code.header.contextSize);
		} else {
			// load thread
			asmMovqMem(buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, thread)), TMP);
			// load dummy context
			asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, offsetof(Thread, context)), TMP);
			// spill dummy context
			asmMovqToMem(buffer, TMP, asmMem(RBP, NO_REGISTER, SS_1, context->frameOffset * sizeof(intptr_t)));

		}
		context->flags |= VAR_ON_STACK;
	}

	ASSERT(context->flags & VAR_IN_REG);
	ASSERT(context->flags & VAR_ON_STACK);
}


static void generateContextRestore(CodeGenerator *generator)
{
	if (generator->code.isBlock || generator->code.header.hasContext) {
		Variable *context = variableAt(generator, CONTEXT_INDEX);
		fillVar(generator, context);
		asmMovqMem(&generator->buffer, asmMem(context->reg, NO_REGISTER, SS_1, varOffset(RawContext, parent)), context->reg);
	}
}


static void generateBody(CodeGenerator *generator)
{
	BytecodesIterator iterator;
	_Bool returns = 0;
	// One label per emitted jump (targets are resolved forward as the iterator
	// reaches them). A jump bytecode is at least 5 bytes, so bytecodesSize is a
	// safe upper bound on the number of jumps — allocate that many to avoid a cap.
	BytecodeLabel *labels = malloc(sizeof(BytecodeLabel) * (generator->code.bytecodesSize + 1));
	BytecodeLabel *currentLabel = labels;

	bytecodeInitIterator(&iterator, generator->code.bytecodes, generator->code.bytecodesSize);
	while (bytecodeHasNext(&iterator)) {
		HandleScope scope;
		openHandleScope(&scope);

		ptrdiff_t offset = bytecodeOffset(&iterator);
		_Bool isJumpTarget = 0;
		BytecodeLabel *label = labels;
		while (label < currentLabel) {
			if (label->offset == offset) {
				asmLabelBind(&generator->buffer, &label->label, asmOffset(&generator->buffer));
				isJumpTarget = 1;
			}
			label++;
		}
		// A jump target is a control-flow merge/branch-entry: code can arrive here
		// from a jump, so no lazily-FILLED register cache can be assumed live (an arm
		// may have filled it on a path that a jump skipped). Drop only the caches
		// that reload from a stable home: stack-homed vars (from their slot) and the
		// context/assoc special vars (re-derived via fillContext/fillAssoc). A
		// register-HOMED temp (VAR_IN_REG with no stack slot) lives in its dedicated
		// register across the whole range and must be kept — clearing it would read
		// an unwritten slot / nil.
		if (isJumpTarget) {
			for (uint8_t i = 0; i < generator->regsAlloc.varsSize; i++) {
				Variable *v = &generator->regsAlloc.vars[i];
				if ((v->flags & VAR_ON_STACK) || v->type != VAR_TMP) {
					v->flags &= ~VAR_IN_REG;
				}
			}
		}

		Bytecode bytecode = bytecodeNext(&iterator);
		generator->bytecodeNumber = bytecodeNumber(&iterator);

		switch (bytecode) {
		case BYTECODE_COPY:
			generateCopy(generator, &iterator);
			break;

		case BYTECODE_SEND:
		case BYTECODE_SEND_WITH_STORE:
			generateSend(generator, &iterator);
			if (bytecode == BYTECODE_SEND_WITH_STORE) {
				movToOperand(generator, RAX, bytecodeNextOperand(&iterator));
			}
			break;

		case BYTECODE_RETURN:
			movOperand(generator, bytecodeNextOperand(&iterator), RAX);
			if (bytecodeHasNext(&iterator)) {
				// Not in tail position (e.g. a `^` inside an inlined conditional
				// arm): jump to the shared epilogue so following code cannot clobber
				// RAX. Marked offset -1 and bound after the fall-through self-load.
				currentLabel->offset = -1;
				asmInitLabel(&currentLabel->label);
				asmJmpLabel(&generator->buffer, &currentLabel->label);
				currentLabel++;
			} else {
				returns = 1;
			}
			break;

		case BYTECODE_OUTER_RETURN:
			returns = 1;
			generateOuterReturn(generator, &iterator);
			break;

		case BYTECODE_JUMP: {
			// The IR stores a relative displacement (target - here - 4); recover the
			// absolute target-bytecode offset by adding the post-read position.
			int32_t disp = bytecodeNextInt32(&iterator);
			currentLabel->offset = bytecodeOffset(&iterator) + disp;
			asmInitLabel(&currentLabel->label);
			asmJmpLabel(&generator->buffer, &currentLabel->label);
			currentLabel++;
			break;
		}

		case BYTECODE_JUMP_NOT_MEMBER_OF: {
			RawObject *class = compiledCodeLiteralAt(&generator->code, bytecodeNextByte(&iterator));
			Operand receiver = bytecodeNextOperand(&iterator);
			int32_t disp = bytecodeNextInt32(&iterator);

			currentLabel->offset = bytecodeOffset(&iterator) + disp;
			asmInitLabel(&currentLabel->label);
			generateClassCheck(generator, receiver, (RawClass *) class, &currentLabel->label);
			currentLabel++;
			break;
		}

		default:
			FAIL();
		}
		closeHandleScope(&scope, NULL);
	}

	// Bind any jumps whose target is the end of the bytecode stream (e.g. an
	// inlined conditional that is the last thing in the method): the in-loop bind
	// only sees bytecode starts, never the one-past-the-end offset.
	for (BytecodeLabel *label = labels; label < currentLabel; label++) {
		if (label->offset == (ptrdiff_t) generator->code.bytecodesSize) {
			asmLabelBind(&generator->buffer, &label->label, asmOffset(&generator->buffer));
		}
	}

	if (!returns) {
		ASSERT(!generator->code.isBlock);
		movVar(generator, variableAt(generator, SELF_INDEX), RAX);
	}

	// Non-tail returns (offset -1) jump here, past the self-load, into the shared
	// method epilogue that generateCode emits next.
	for (BytecodeLabel *label = labels; label < currentLabel; label++) {
		if (label->offset == -1) {
			asmLabelBind(&generator->buffer, &label->label, asmOffset(&generator->buffer));
		}
	}

	free(labels);
}


static void generateCopy(CodeGenerator *generator, BytecodesIterator *iterator)
{
	Operand src = bytecodeNextOperand(iterator);
	Operand dst = bytecodeNextOperand(iterator);
	if (src.type == OPERAND_TEMP_VAR || src.type == OPERAND_ARG_VAR) {
		Variable *srcVar = variableAt(generator, src.index);
		if (srcVar->flags & (VAR_IN_REG | VAR_ON_STACK)) {
			fillVar(generator, srcVar);
		} else {
			generateLoadObject(&generator->buffer, Handles.nil->raw, srcVar->reg, 1);
			srcVar->flags |= VAR_IN_REG;
		}
		movToOperand(generator, srcVar->reg, dst);
	} else if (dst.type == OPERAND_TEMP_VAR) {
		Variable *dstVar = variableAt(generator, dst.index);
		if (dstVar->reg == SPILLED_REG) {
			movOperand(generator, src, RAX);
			asmMovqToMem(&generator->buffer, RAX, asmMem(RBP, NO_REGISTER, SS_1, dstVar->frameOffset * sizeof(intptr_t)));
			dstVar->flags |= VAR_ON_STACK;
		} else {
			movOperand(generator, src, dstVar->reg);
			dstVar->flags |= VAR_IN_REG;
			spillVar(generator, dstVar);
		}
	} else {
		movOperand(generator, src, RAX);
		movToOperand(generator, RAX, dst);
	}
}


// SmallInteger selectors inlined at the call site (fast path in generateSend).
enum { ARITH_NONE = 0, ARITH_ADD, ARITH_SUB, ARITH_MUL,
       ARITH_LT, ARITH_LE, ARITH_GT, ARITH_GE, ARITH_EQ, ARITH_NE };

static int classifyArith(RawObject *selector)
{
	size_t n = rawObjectSize(selector);
	char *s = (char *) getRawObjectIndexedVars(selector);
	if (n == 1) {
		switch (s[0]) {
		case '+': return ARITH_ADD;
		case '-': return ARITH_SUB;
		case '*': return ARITH_MUL;
		case '<': return ARITH_LT;
		case '>': return ARITH_GT;
		case '=': return ARITH_EQ;
		}
	} else if (n == 2 && s[1] == '=') {
		switch (s[0]) {
		case '<': return ARITH_LE;
		case '>': return ARITH_GE;
		case '~': return ARITH_NE;
		}
	}
	return ARITH_NONE;
}

static _Bool arithIsCompare(int kind) { return kind >= ARITH_LT; }


// TMP: receiver
// RDI: receiver class
// RSI: selector
// RAX: result from invoked method
static void generateSend(CodeGenerator *generator, BytecodesIterator *iterator)
{
	//addSourceCodeDescriptor(generator, p - start);
	//spillRegs(generator, offset);

	AssemblerBuffer *buffer = &generator->buffer;
	RawObject *selector = compiledCodeLiteralAt(&generator->code, bytecodeNextByte(iterator));
	uint8_t argsSize = bytecodeNextByte(iterator);
	Operand receiver = bytecodeNextOperand(iterator);

	for (uint8_t i = 0; i < argsSize; i++) {
		pushOperand(generator, bytecodeNextOperand(iterator));
	}
	movOperand(generator, receiver, TMP);
	asmPushq(buffer, TMP);
	generator->frameSize++;

	// --- inline SmallInteger arithmetic fast path ---------------------------
	// Operands are on the stack (receiver at [rsp], arg at [rsp+8]); if both are
	// SmallIntegers do the tagged op inline and skip the dispatch, otherwise fall
	// through to the normal send (which handles overflow/coercion via retry:).
	// Uses ONLY RAX/RSI as scratch — both are re-established by the dispatch path —
	// and leaves TMP (= receiver) untouched, so the class computation below is
	// unaffected. Two separate tag tests avoid needing a third scratch register.
	int arithKind = argsSize == 1 ? classifyArith(selector) : ARITH_NONE;
	AssemblerLabel arithMerge, tagMissR, tagMissA, overflowMiss;
	if (arithKind != ARITH_NONE) {
		asmInitLabel(&arithMerge);
		asmInitLabel(&tagMissR);
		asmInitLabel(&tagMissA);
		asmInitLabel(&overflowMiss);
		asmMovqMem(buffer, asmMem(RSP, NO_REGISTER, SS_1, 0), RAX);                    // receiver
		asmMovqMem(buffer, asmMem(RSP, NO_REGISTER, SS_1, sizeof(intptr_t)), RSI);     // arg
		asmTestqImm(buffer, RAX, 3);                                                    // receiver SmallInt?
		asmJ(buffer, COND_NOT_ZERO, &tagMissR);
		asmTestqImm(buffer, RSI, 3);                                                    // arg SmallInt?
		asmJ(buffer, COND_NOT_ZERO, &tagMissA);
		switch (arithKind) {
		case ARITH_ADD:
			asmAddq(buffer, RSI, RAX);
			asmJ(buffer, COND_OVERFLOW, &overflowMiss);                                 // -> LargeInteger via send
			break;
		case ARITH_SUB:
			asmSubq(buffer, RSI, RAX);
			asmJ(buffer, COND_OVERFLOW, &overflowMiss);
			break;
		case ARITH_MUL:
			asmSarqImm(buffer, RAX, 2);                                                 // untag receiver -> a
			asmImulq(buffer, RSI, RAX);                                                 // a * (b<<2) = (a*b)<<2
			asmJ(buffer, COND_OVERFLOW, &overflowMiss);
			break;
		default: {   // comparisons: cmp then branch on FRESH flags (loads clobber flags)
			AssemblerLabel cmpTrue, cmpDone;
			uint8_t cond = arithKind == ARITH_LT ? COND_LESS
				: arithKind == ARITH_LE ? COND_LESS_EQUAL
				: arithKind == ARITH_GT ? COND_GREATER
				: arithKind == ARITH_GE ? COND_GREATER_EQUAL
				: arithKind == ARITH_EQ ? COND_EQUAL
				: COND_NOT_EQUAL;
			asmInitLabel(&cmpTrue);
			asmInitLabel(&cmpDone);
			asmCmpq(buffer, RAX, RSI);                                                  // cmp receiver, arg (receiver - arg)
			asmJ(buffer, cond, &cmpTrue);
			generateLoadObject(buffer, Handles.false->raw, RAX, 1);
			asmJmpLabel(buffer, &cmpDone);
			asmLabelBind(buffer, &cmpTrue, asmOffset(buffer));
			generateLoadObject(buffer, Handles.true->raw, RAX, 1);
			asmLabelBind(buffer, &cmpDone, asmOffset(buffer));
			break;
		}
		}
		asmJmpLabel(buffer, &arithMerge);
		asmLabelBind(buffer, &tagMissR, asmOffset(buffer));
		asmLabelBind(buffer, &tagMissA, asmOffset(buffer));
		asmLabelBind(buffer, &overflowMiss, asmOffset(buffer));
	}

	RawClass *class = compiledCodeResolveOperandClass(&generator->code, receiver);
	if (class != NULL) {
		asmMovqImm(buffer, (int64_t) lookupNativeCode(class, (RawString *) selector), R11);
	} else {
		// Always recompute the receiver's class from TMP. The old VAR_CLASS spill
		// cache (keyed by the receiver variable) is UNSAFE once inlined control flow
		// exists: a send inside a conditional arm would spill the class on a path
		// that is not always taken, and a later send would read the unpopulated slot
		// as a garbage class. Recomputing per send is a few instructions and correct.
		generateLoadClass(buffer, TMP, RDI);
		generateLoadObject(buffer, selector, RSI, 0);
		generateMethodLookup(generator);
	}

	generator->frameSize -= argsSize + 1;
	asmCallq(buffer, R11);
	generateStackmap(generator);
	ordCollAdd(generator->descriptors, createBytecodeDescriptor(asmOffset(&generator->buffer), generator->bytecodeNumber));
	// Both the inline fast path and the dispatched call converge here (result in
	// RAX) so they share the single arg-pop.
	if (arithKind != ARITH_NONE) {
		asmLabelBind(buffer, &arithMerge, asmOffset(buffer));
	}
	asmAddqImm(buffer, RSP, (argsSize + 1) * sizeof(intptr_t));
	invalidateRegs(&generator->regsAlloc);

	// The dispatch path above computes the receiver's class and SPILLS it to the
	// VAR_CLASS frame slot so later sends to the same variable can reuse it. Our
	// fast path skips that spill at runtime (it jumps straight to the merge), so
	// the slot would be left unpopulated. Forget the cached/spilled class for this
	// receiver so the next send recomputes it from the receiver instead of reading
	// the stale slot.
	if (arithKind != ARITH_NONE &&
	    (receiver.type == OPERAND_TEMP_VAR || receiver.type == OPERAND_ARG_VAR)) {
		Variable *classVar = specialVariableAt(generator, VAR_CLASS, receiver.index);
		classVar->flags &= ~(VAR_ON_STACK | VAR_IN_REG);
	}
}


void generateStoreCheck(CodeGenerator *generator, Register object, Register value)
{
	ASSERT(object != TMP && value != TMP);
	MemoryOperand tags = asmMem(object, NO_REGISTER, SS_1, varOffset(RawObject, tags));
	AssemblerBuffer *buffer = &generator->buffer;
	// Each AssemblerLabel supports a single forward reference, so every jump
	// needs its own label; the skips are all bound to the exit point below.
	AssemblerLabel objectIsNew;
	AssemblerLabel valueIsNotPtr;
	AssemblerLabel valueIsOld;
	AssemblerLabel alreadyInSet;
	AssemblerLabel notFull;

	asmInitLabel(&objectIsNew);
	asmInitLabel(&valueIsNotPtr);
	asmInitLabel(&valueIsOld);
	asmInitLabel(&alreadyInSet);
	asmInitLabel(&notFull);

	ptrdiff_t rememberedSetOffset = offsetof(Thread, heap) + offsetof(Heap, rememberedSet);
	ptrdiff_t blocksOffset = rememberedSetOffset + offsetof(RememberedSet, blocks);
	MemoryOperand blockCurrent = asmMem(TMP, NO_REGISTER, SS_1, offsetof(RememberedSetBlock, current));
	MemoryOperand blockEnd = asmMem(TMP, NO_REGISTER, SS_1, offsetof(RememberedSetBlock, end));

	// Fast-path skips: only an old object gaining a pointer to a young object
	// that is not already remembered needs recording.
	asmTestqImm(buffer, object, NEW_SPACE_TAG);   // object is young -> nothing to do
	asmJ(buffer, COND_NOT_ZERO, &objectIsNew);
	asmTestqImm(buffer, value, VALUE_POINTER);    // value is not a pointer
	asmJ(buffer, COND_ZERO, &valueIsNotPtr);
	asmTestqImm(buffer, value, NEW_SPACE_TAG);    // value is old
	asmJ(buffer, COND_ZERO, &valueIsOld);
	asmTestbMemImm(buffer, tags, TAG_REMEMBERED); // object already remembered
	asmJ(buffer, COND_NOT_ZERO, &alreadyInSet);

	// mark as remembered
	asmOrbMemImm(buffer, tags, TAG_REMEMBERED);

	// TMP = remembered set head block
	asmMovqMem(buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, thread)), TMP);
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, blocksOffset), TMP);

	// Grow BEFORE the store when the block is full (current >= end), so the store
	// is always in bounds. `cmp end, [current]` leaves end > current (room) as
	// COND_ABOVE. (RAX is only a scratch for `end`; push/pop keeps it intact even
	// if object/value happen to live in it.) Growing AFTER the store — as the old
	// inline code did on a mis-read condition — writes one slot past objects[]
	// and corrupts the C heap under sustained load.
	asmPushq(buffer, RAX);
	asmMovqMem(buffer, blockEnd, RAX);              // RAX = block->end
	asmCmpqMem(buffer, blockCurrent, RAX);          // compare end with block->current
	asmPopq(buffer, RAX);
	asmJ(buffer, COND_ABOVE, &notFull);             // end > current -> room, skip grow

	// grow (rare: once per 1024 remembered objects) via the C helper, then reload
	asmPushq(buffer, RAX);
	asmPushq(buffer, RCX);
	asmPushq(buffer, RDX);
	asmPushq(buffer, RSI);
	asmPushq(buffer, RDI);
	asmPushq(buffer, R8);
	asmPushq(buffer, R9);
	asmPushq(buffer, R11);
	asmMovqMem(buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, thread)), TMP);
	asmLeaq(buffer, asmMem(TMP, NO_REGISTER, SS_1, rememberedSetOffset), RDI);
	generateCCall(generator, (intptr_t) rememberedSetGrow, 1, 0);
	asmPopq(buffer, R11);
	asmPopq(buffer, R9);
	asmPopq(buffer, R8);
	asmPopq(buffer, RDI);
	asmPopq(buffer, RSI);
	asmPopq(buffer, RDX);
	asmPopq(buffer, RCX);
	asmPopq(buffer, RAX);
	asmMovqMem(buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, thread)), TMP);
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, blocksOffset), TMP);

	asmLabelBind(buffer, &notFull, asmOffset(buffer));
	// store: advance block->current in memory, then write object at the old slot.
	// Uses only TMP as scratch (never `object`/`value`, which may alias).
	asmAddqMemImm(buffer, blockCurrent, sizeof(intptr_t));  // block->current += 8
	asmMovqMem(buffer, blockCurrent, TMP);          // TMP = block->current (advanced)
	asmMovqToMem(buffer, object, asmMem(TMP, NO_REGISTER, SS_1, -(ptrdiff_t) sizeof(intptr_t)));  // *(current-8) = object

	asmLabelBind(buffer, &objectIsNew, asmOffset(buffer));
	asmLabelBind(buffer, &valueIsNotPtr, asmOffset(buffer));
	asmLabelBind(buffer, &valueIsOld, asmOffset(buffer));
	asmLabelBind(buffer, &alreadyInSet, asmOffset(buffer));
}


// RDI: class
// RSI: selector
// RDX: class and selector hash
// R11: native code
void generateMethodLookup(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel lookup;
	AssemblerLabel cache;
	AssemblerLabel call;

	asmInitLabel(&lookup);
	asmInitLabel(&cache);
	asmInitLabel(&call);

	asmDecq(buffer, RDI);

	// hash class and selector
	asmMovq(buffer, RDI, RDX);
	asmXorq(buffer, RSI, RDX);
	asmAndqImm(buffer, RDX, LOOKUP_CACHE_SIZE - 1);

	// check class
	asmMovqImm(buffer, (uint64_t) &LookupCache, TMP);
	asmCmpqMem(buffer, asmMem(TMP, RDX, SS_8, offsetof(LookupTable, classes)), RDI);
	asmJ(buffer, COND_NOT_EQUAL, &lookup);

	// check selector
	asmCmpqMem(buffer, asmMem(TMP, RDX, SS_8, offsetof(LookupTable, selectors)), RSI);
	asmJ(buffer, COND_EQUAL, &cache);

	// not in cache -> lookup
	asmLabelBind(buffer, &lookup, asmOffset(buffer));
	generateStubCall(generator, &LookupStub);
	asmJmpLabel(buffer, &call);

	// load code from cache
	asmLabelBind(buffer, &cache, asmOffset(buffer));
	asmMovqMem(buffer, asmMem(TMP, RDX, SS_8, offsetof(LookupTable, codes)), R11);

	asmLabelBind(buffer, &call, asmOffset(buffer));
}


void generateLoadObject(AssemblerBuffer *buffer, RawObject *object, Register dst, _Bool tag)
{
	int64_t ptr = tag ? tagPtr(object) : (int64_t) object;
	asmMovqImm(buffer, ptr, dst);
	if (!isOldObject(object)) {
		asmAddPointerOffset(buffer, asmOffset(buffer) - sizeof(int64_t));
	}
}


void generateLoadClass(AssemblerBuffer *buffer, Register src, Register dst)
{
	AssemblerLabel pointer;
	AssemblerLabel character;
	AssemblerLabel end, end2;

	asmInitLabel(&pointer);
	asmInitLabel(&character);
	asmInitLabel(&end);
	asmInitLabel(&end2);

	asmMovq(buffer, src, dst);
	asmAndqImm(buffer, dst, 3);
	asmCmpqImm(buffer, dst, VALUE_POINTER);
	asmJ(buffer, COND_ABOVE, &character);
	asmJ(buffer, COND_EQUAL, &pointer);

	generateLoadObject(buffer, (RawObject *) Handles.SmallInteger->raw, dst, 1);
	asmJmpLabel(buffer, &end);

	asmLabelBind(buffer, &pointer, asmOffset(buffer));
	asmMovqMem(buffer, asmMem(src, NO_REGISTER, SS_1, -1), dst);
	asmIncq(buffer, dst);
	asmJmpLabel(buffer, &end2);

	asmLabelBind(buffer, &character, asmOffset(buffer));
	generateLoadObject(buffer, (RawObject *) Handles.Character->raw, dst, 1);

	asmLabelBind(buffer, &end, asmOffset(buffer));
	asmLabelBind(buffer, &end2, asmOffset(buffer));
}


static void generateOuterReturn(CodeGenerator *generator, BytecodesIterator *iterator)
{
	AssemblerLabel deathContext;
	asmInitLabel(&deathContext);

	Variable *context = variableAt(generator, CONTEXT_INDEX);
	AssemblerBuffer *buffer = &generator->buffer;

	movOperand(generator, bytecodeNextOperand(iterator), RAX);

	fillVar(generator, context);
	// load home context
	asmMovqMem(buffer, asmMem(context->reg, NO_REGISTER, SS_1, varOffset(RawContext, home)), context->reg);
	// load home stack frame
	asmMovqMem(buffer, asmMem(context->reg, NO_REGISTER, SS_1, varOffset(RawContext, frame)), TMP);
	// check frame validity
	asmCmpqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), context->reg);
	asmJ(buffer, COND_NOT_EQUAL, &deathContext);

	// context is a live so return
	asmMovq(buffer, TMP, RSP);
	asmPopq(buffer, RBP);
	asmRet(buffer);

	// death context
	asmLabelBind(buffer, &deathContext, asmOffset(buffer));
	// invoke #cannotReturn:
	asmPushq(buffer, RAX);
	asmPushq(buffer, context->reg);
	generator->frameSize += 2;
	// TODO: missed stackFrameSize update?
	generateLoadObject(buffer, (RawObject *) Handles.MethodContext->raw, RDI, 1);
	generateLoadObject(buffer, (RawObject *) Handles.cannotReturnSymbol->raw, RSI, 0);
	generateMethodLookup(generator);
	generator->frameSize -= 2;
	asmCallq(buffer, R11);
	generateStackmap(generator);
	asmAddqImm(buffer, RSP, 2 * sizeof(intptr_t));
}


static void pushOperand(CodeGenerator *generator, Operand operand)
{
	AssemblerBuffer *buffer = &generator->buffer;

	switch (operand.type) {
	case OPERAND_VALUE:
		if (INT32_MIN <= operand.int64 && operand.int64 <= INT32_MAX) {
			asmPushqImm(buffer, operand.value);
		} else {
			asmMovqImm(buffer, operand.value, TMP);
			asmPushq(buffer, TMP);
		}
		break;

	case OPERAND_NIL:
		generateLoadObject(buffer, Handles.nil->raw, TMP, 1);
		asmPushq(buffer, TMP);
		break;

	case OPERAND_TRUE:
		generateLoadObject(buffer, Handles.true->raw, TMP, 1);
		asmPushq(buffer, TMP);
		break;

	case OPERAND_FALSE:
		generateLoadObject(buffer, Handles.false->raw, TMP, 1);
		asmPushq(buffer, TMP);
		break;

	case OPERAND_THIS_CONTEXT: {
		Variable *context = variableAt(generator, CONTEXT_INDEX);
		fillVar(generator, context);
		asmPushq(buffer, context->reg);
		break;
	}

	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:;
		Variable *var = variableAt(generator, operand.index);
		if (var->flags & VAR_IN_REG) {
			asmPushq(buffer, var->reg);
		} else if (var->flags & VAR_ON_STACK) {
			asmPushqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, var->frameOffset * sizeof(intptr_t)));
		} else if (var->reg == SPILLED_REG) {
			generateLoadObject(buffer, Handles.nil->raw, TMP, 1);
			asmPushq(buffer, TMP);
		} else {
			generateLoadObject(buffer, Handles.nil->raw, var->reg, 1);
			var->flags |= VAR_IN_REG;
			asmPushq(buffer, var->reg);
		}
		break;

	case OPERAND_SUPER: {
		Variable *self = variableAt(generator, SELF_INDEX);
		fillVar(generator, self);
		asmPushq(buffer, self->reg);
		break;
	}

	case OPERAND_CONTEXT_VAR: {
		Variable *context = specialVariableAt(generator, VAR_CONTEXT, operand.level);
		ptrdiff_t offset = varOffset(RawContext, vars) + operand.index * sizeof(Value);
		fillContext(generator, operand.level);
		asmPushqMem(buffer, asmMem(context->reg, NO_REGISTER, SS_1, offset));
		break;
	}

	case OPERAND_INST_VAR: {
		Variable *self = variableAt(generator, SELF_INDEX);
		InstanceShape shape = classGetInstanceShape(generator->code.ownerClass);
		ptrdiff_t offset = varOffset(RawObject, body) + (shape.payloadSize + operand.index + shape.isIndexed) * sizeof(Value);
		fillVar(generator, self);
		asmPushqMem(buffer, asmMem(self->reg, NO_REGISTER, SS_1, offset));
		break;
	}

	case OPERAND_LITERAL:
		generateLoadObject(buffer, compiledCodeLiteralAt(&generator->code, operand.index), TMP, 1);
		asmPushq(buffer, TMP);
		break;

	case OPERAND_ASSOC:;
		generateLoadObject(buffer, compiledCodeLiteralAt(&generator->code, operand.index), TMP, 1);
		asmPushqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, varOffset(RawAssociation, value)));
		break;

	case OPERAND_BLOCK:;
		generateLoadBlock(generator, operand);
		asmPushq(buffer, RAX);
		break;

	default:
		FAIL();
	}

	generator->frameSize++;
}


static void movOperand(CodeGenerator *generator, Operand operand, Register reg)
{
	AssemblerBuffer *buffer = &generator->buffer;

	switch (operand.type) {
	case OPERAND_VALUE:
		asmMovqImm(buffer, operand.value, reg);
		break;

	case OPERAND_NIL:
		generateLoadObject(buffer, Handles.nil->raw, reg, 1);
		break;

	case OPERAND_TRUE:
		generateLoadObject(buffer, Handles.true->raw, reg, 1);
		break;

	case OPERAND_FALSE:
		generateLoadObject(buffer, Handles.false->raw, reg, 1);
		break;

	case OPERAND_THIS_CONTEXT:
		movVar(generator, variableAt(generator, CONTEXT_INDEX), reg);
		break;

	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:
		movVar(generator, variableAt(generator, operand.index), reg);
		break;

	case OPERAND_SUPER:
		movVar(generator, variableAt(generator, SELF_INDEX), reg);
		break;

	case OPERAND_CONTEXT_VAR: {
		Variable *context = specialVariableAt(generator, VAR_CONTEXT, operand.level);
		ptrdiff_t offset = varOffset(RawContext, vars) + operand.index * sizeof(Value);
		fillContext(generator, operand.level);
		asmMovqMem(buffer, asmMem(context->reg, NO_REGISTER, SS_1, offset), reg);
		break;
	}

	case OPERAND_INST_VAR: {
		Variable *self = variableAt(generator, SELF_INDEX);
		InstanceShape shape = classGetInstanceShape(generator->code.ownerClass);
		ptrdiff_t offset = varOffset(RawObject, body) + (shape.payloadSize + operand.index + shape.isIndexed) * sizeof(Value);
		fillVar(generator, self);
		asmMovqMem(buffer, asmMem(self->reg, NO_REGISTER, SS_1, offset), reg);
		break;
	}

	case OPERAND_LITERAL:
		generateLoadObject(buffer, compiledCodeLiteralAt(&generator->code, operand.index), reg, 1);
		break;

	case OPERAND_ASSOC: {
		Variable *variable = specialVariableAt(generator, VAR_ASSOC, operand.index);
		Register src;
		if (variable->reg == SPILLED_REG) {
			src = reg;
			generateLoadObject(&generator->buffer, compiledCodeLiteralAt(&generator->code, operand.index), src, 1);
		} else {
			fillAssoc(generator, operand.index);
			src = variable->reg;
		}
		asmMovqMem(buffer, asmMem(src, NO_REGISTER, SS_1, varOffset(RawAssociation, value)), reg);
		break;
	}

	case OPERAND_BLOCK:
		generateLoadBlock(generator, operand);
		asmMovq(buffer, RAX, reg);
		break;

	default:
		FAIL();
	}
}


static void movToOperand(CodeGenerator *generator, Register reg, Operand operand)
{
	AssemblerBuffer *buffer = &generator->buffer;

	switch (operand.type) {
	case OPERAND_VALUE:
	case OPERAND_NIL:
	case OPERAND_TRUE:
	case OPERAND_FALSE:
	case OPERAND_THIS_CONTEXT:
		FAIL();
		break;

	case OPERAND_TEMP_VAR:
		movToVar(generator, reg, variableAt(generator, operand.index));
		break;

	case OPERAND_CONTEXT_VAR: {
		Variable *context = specialVariableAt(generator, VAR_CONTEXT, operand.level);
		ptrdiff_t offset = varOffset(RawContext, vars) + operand.index * sizeof(Value);
		fillContext(generator, operand.level);
		generateStoreCheck(generator, context->reg, reg);
		asmMovqToMem(buffer, reg, asmMem(context->reg, NO_REGISTER, SS_1, offset));
		break;
	}

	case OPERAND_INST_VAR: {
		Variable *self = variableAt(generator, SELF_INDEX);
		InstanceShape shape = classGetInstanceShape(generator->code.ownerClass);
		ptrdiff_t offset = varOffset(RawObject, body) + (shape.payloadSize + operand.index + shape.isIndexed) * sizeof(Value);

		fillVar(generator, self);
		generateStoreCheck(generator, self->reg, reg);
		asmMovqToMem(buffer, reg, asmMem(self->reg, NO_REGISTER, SS_1, offset));
		break;
	}

	case OPERAND_ASSOC: {
		Variable *variable = specialVariableAt(generator, VAR_ASSOC, operand.index);
		fillAssoc(generator, operand.index);
		asmMovqToMem(buffer, reg, asmMem(variable->reg, NO_REGISTER, SS_1, varOffset(RawAssociation, value)));
		generateStoreCheck(generator, variable->reg, reg);
		break;
	}

	default:
		FAIL();
	}
}


static void generateClassCheck(CodeGenerator *generator, Operand operand, RawClass *class, AssemblerLabel *label)
{
	AssemblerBuffer *buffer = &generator->buffer;

	switch (operand.type) {
	case OPERAND_VALUE:
		if (class != getClassOf(operand.value)) {
			asmJmpLabel(buffer, label);
		}
		break;

	case OPERAND_NIL:
		if (class != Handles.UndefinedObject->raw) {
			asmJmpLabel(buffer, label);
		}
		break;

	case OPERAND_TRUE:
		if (class != Handles.True->raw) {
			asmJmpLabel(buffer, label);
		}
		break;

	case OPERAND_FALSE:
		if (class != Handles.False->raw) {
			asmJmpLabel(buffer, label);
		}
		break;

	case OPERAND_THIS_CONTEXT: {
		RawClass *contextClass = (generator->code.isBlock ? Handles.BlockContext : Handles.MethodContext)->raw;
		if (class != contextClass) {
			asmJmpLabel(buffer, label);
		}
		break;
	}

	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR: {
		Variable *var = variableAt(generator, operand.index);

		if (class == Handles.SmallInteger->raw) {
			fillVar(generator, var);
			asmTestqImm(buffer, var->reg, 3);
			asmJ(buffer, COND_NOT_ZERO, label);
		} else if (class == Handles.Character->raw) {
			fillVar(generator, var);
			asmTestqImm(buffer, var->reg, VALUE_CHAR);
			asmJ(buffer, COND_ZERO, label);
		} else {
			// Compute the receiver's class exactly like generateSend does
			// (generateLoadClass: correct tag handling, tagged result) into scratch
			// RAX, and compare to the tested class in TMP. The VAR_CLASS cache is
			// deliberately NOT touched here — generateSend owns it and stores it in
			// a form this path must not corrupt.
			fillVar(generator, var);
			generateLoadClass(buffer, var->reg, RAX);
			generateLoadObject(buffer, (RawObject *) class, TMP, 1);
			asmCmpq(buffer, RAX, TMP);
			asmJ(buffer, COND_NOT_EQUAL, label);
		}
		break;
	}

	case OPERAND_SUPER:
		if (class != (RawClass *) asObject(generator->code.ownerClass->raw->superClass)) {
			asmJmpLabel(buffer, label);
		}
		break;

	case OPERAND_CONTEXT_VAR: {
		Variable *context = specialVariableAt(generator, VAR_CONTEXT, operand.level);
		ptrdiff_t offset = varOffset(RawContext, vars) + operand.index * sizeof(Value);
		fillContext(generator, operand.level);
		asmMovqMem(buffer, asmMem(context->reg, NO_REGISTER, SS_1, offset), TMP);

		if (class == Handles.SmallInteger->raw) {
			asmTestqImm(buffer, TMP, 3);
			asmJ(buffer, COND_NOT_ZERO, label);
		} else if (class == Handles.Character->raw) {
			asmTestqImm(buffer, TMP, VALUE_CHAR);
			asmJ(buffer, COND_ZERO, label);
		} else {
			generateLoadObject(buffer, (RawObject *) class, RAX, 0);
			asmCmpqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, varOffset(RawObject, class)), RAX);
			asmJ(buffer, COND_NOT_EQUAL, label);
		}
		break;
	}

	case OPERAND_INST_VAR: {
		Variable *self = variableAt(generator, SELF_INDEX);
		InstanceShape shape = classGetInstanceShape(generator->code.ownerClass);
		ptrdiff_t offset = varOffset(RawObject, body) + (shape.payloadSize + operand.index + shape.isIndexed) * sizeof(Value);
		fillVar(generator, self);

		asmMovqMem(buffer, asmMem(self->reg, NO_REGISTER, SS_1, offset), TMP);

		if (class == Handles.SmallInteger->raw) {
			asmTestqImm(buffer, TMP, 3);
			asmJ(buffer, COND_NOT_ZERO, label);
		} else if (class == Handles.Character->raw) {
			asmTestqImm(buffer, TMP, VALUE_CHAR);
			asmJ(buffer, COND_ZERO, label);
		} else {
			generateLoadObject(buffer, (RawObject *) class, RAX, 0);
			asmCmpqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, varOffset(RawObject, class)), RAX);
			asmJ(buffer, COND_NOT_EQUAL, label);
		}
		break;
	}

	case OPERAND_LITERAL:
	case OPERAND_ASSOC: {
		RawObject *literal = compiledCodeLiteralAt(&generator->code, operand.index);
		if (class != literal->class) {
			asmJmpLabel(buffer, label);
		}
		break;
	}

	case OPERAND_BLOCK:
		if (class != Handles.Block->raw) {
			asmJmpLabel(buffer, label);
		}
		break;

	default:
		FAIL();
	}
}


// RAX: block
static void generateLoadBlock(CodeGenerator *generator, Operand operand)
{
	HandleScope scope;
	openHandleScope(&scope);

	CompiledBlock *block = scopeHandle(compiledCodeLiteralAt(&generator->code, operand.index));
	NativeCode *nativeBlock = generateBlockCode(block, generator);
	Variable *context = variableAt(generator, CONTEXT_INDEX);
	AssemblerBuffer *buffer = &generator->buffer;

	// allocate a Block
	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, RSI, 0);
	asmMovqImm(buffer, 0, RDX);
	generateStubCall(generator, &AllocateStub);
	invalidateRegs(&generator->regsAlloc);

	// setup home context
	fillVar(generator, context);
	if (generator->code.isBlock) {
		asmMovqMem(buffer, asmMem(context->reg, NO_REGISTER, SS_1, varOffset(RawContext, home)), TMP);
		asmMovqToMem(buffer, TMP, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawBlock, homeContext)));
	} else {
		asmMovqToMem(buffer, context->reg, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawBlock, homeContext)));
	}

	// setup native code
	asmMovqImm(buffer, (uint64_t) nativeBlock, TMP); // TODO: native code can be reallocated?
	asmMovqToMem(buffer, TMP, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawBlock, nativeCode)));

	// setup compiled code
	generateLoadObject(buffer, (RawObject *) block->raw, TMP, 1);
	asmMovqToMem(buffer, TMP, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawBlock, compiledBlock)));

	// setup receiver
	Variable *self = variableAt(generator, SELF_INDEX);
	fillVar(generator, self);
	asmMovqToMem(buffer, self->reg, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawBlock, receiver)));

	// setup outer context
	asmMovqToMem(buffer, context->reg, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawBlock, outerContext)));

	closeHandleScope(&scope, NULL);
}


static void fillContext(CodeGenerator *generator, uint8_t level)
{
	Variable *context = specialVariableAt(generator, VAR_CONTEXT, level);
	if (context->flags & VAR_IN_REG) {
		// nothing
	} else if (level == 0 && (context->flags & VAR_ON_STACK)) {
		// Only the level-0 context has a slot that is ALWAYS written (in the
		// prologue), so it is safe to reload from the stack. An outer (level>0)
		// context is spilled only on the path that walked it, so across control flow
		// its slot may be unwritten — fall through and re-walk it from level 0.
		fillVar(generator, context);
	} else {
		Variable *outer = specialVariableAt(generator, VAR_CONTEXT, 0);
		fillVar(generator, outer);
		for (uint8_t i = 0; i < level; i++) {
			asmMovqMem(&generator->buffer, asmMem(outer->reg, NO_REGISTER, SS_1, varOffset(RawContext, outer)), context->reg);
			outer = context;
		}
		context->flags |= VAR_IN_REG;
		// Do NOT spill an outer (level>0) context to a frame slot: it is always
		// re-walked from level 0 (never reloaded), so a spill would only leave a
		// conditionally-written slot that the GC stackmap scans as a bogus root. It
		// is not a live root across a send either — invalidateRegs after each send
		// drops the register and it is re-walked afresh.
		if (level == 0) {
			spillVar(generator, context);
		}
	}
}


static void fillAssoc(CodeGenerator *generator, uint8_t index)
{
	Variable *var = specialVariableAt(generator, VAR_ASSOC, index);
	if ((var->flags & VAR_IN_REG) == 0) {
		ASSERT(var->reg != SPILLED_REG);
		generateLoadObject(&generator->buffer, compiledCodeLiteralAt(&generator->code, index), var->reg, 1);
		var->flags |= VAR_IN_REG;
	}
}


static void fillVar(CodeGenerator *generator, Variable *var)
{
	if ((var->flags & VAR_IN_REG) == 0) {
		ASSERT(var->flags & VAR_ON_STACK);
		ptrdiff_t offset = var->frameOffset * sizeof(intptr_t);
		asmMovqMem(&generator->buffer, asmMem(generator->regsAlloc.frameLess ? RSP : RBP, NO_REGISTER, SS_1, offset), var->reg);
		var->flags |= VAR_IN_REG;
	}
}


static void spillVar(CodeGenerator *generator, Variable *var)
{
	ASSERT(var->flags & VAR_IN_REG);
	ptrdiff_t offset = var->frameOffset * sizeof(intptr_t);
	asmMovqToMem(&generator->buffer, var->reg, asmMem(RBP, NO_REGISTER, SS_1, offset));
	var->flags |= VAR_ON_STACK;
}


static void movVar(CodeGenerator *generator, Variable *var, Register reg)
{
	if (var->reg == SPILLED_REG && (var->flags & VAR_ON_STACK) != 0) {
		asmMovqMem(&generator->buffer, asmMem(RBP, NO_REGISTER, SS_1, var->frameOffset * sizeof(intptr_t)), reg);
	} else if (var->flags & (VAR_IN_REG | VAR_ON_STACK)) {
		ASSERT(var->reg != reg);
		fillVar(generator, var);
		asmMovq(&generator->buffer, var->reg, reg);
	} else {
		generateLoadObject(&generator->buffer, Handles.nil->raw, reg, 1);
	}
}


static void movToVar(CodeGenerator *generator, Register reg, Variable *var)
{
	if (var->reg == SPILLED_REG) {
		var->flags |= VAR_ON_STACK;
		asmMovqToMem(&generator->buffer, reg, asmMem(RBP, NO_REGISTER, SS_1, var->frameOffset * sizeof(intptr_t)));
	} else {
		ASSERT(reg != var->reg);
		var->flags |= VAR_IN_REG;
		asmMovq(&generator->buffer, reg, var->reg);
		spillVar(generator, var);
	}
	Variable *classVar = specialVariableAt(generator, VAR_CLASS, var->index);
	if (classVar != NULL) {
		classVar->flags &= ~(VAR_ON_STACK | VAR_IN_REG);
	}
}


static Variable *variableAt(CodeGenerator *generator, ptrdiff_t index)
{
	return &generator->regsAlloc.vars[index];
}


static Variable *specialVariableAt(CodeGenerator *generator, uint8_t type, ptrdiff_t index)
{
	//if (generator->regsAlloc.specialVars[type][index] != NULL && generator->regsAlloc.specialVars[type][index]->reg == SPILLED_REG) {
	//	asm("int3");
	//}
	return generator->regsAlloc.specialVars[type][index];
}


void generateStackmap(CodeGenerator *generator)
{
	HandleScope scope;
	openHandleScope(&scope);

	size_t size = (generator->frameSize + generator->frameRawAreaSize) / 8 + 1 + sizeof(Value);
	Stackmap *stackmap = newObject(Handles.ByteArray, size);
	stackmap->raw->ic = asmOffset(&generator->buffer);

	size_t varsSize = generator->regsAlloc.varsSize;
	for (size_t i = 0; i < varsSize; i++) {
		Variable *var = variableAt(generator, i);
		if (i == CONTEXT_INDEX || (var->frameOffset < 0 && (var->flags & VAR_ON_STACK) && var->start <= generator->bytecodeNumber && generator->bytecodeNumber <= var->end)) {
			ASSERT(var->frameOffset < -1);
			size_t index = -var->frameOffset - 1;
			if (index > 1) {
				index += generator->frameRawAreaSize;
			}
			stackmapAdd(stackmap->raw, index);
		}
	}

	size_t extraFrameSize = generator->frameSize;
	for (size_t i = generator->regsAlloc.frameSize; i < extraFrameSize; i++) {
		stackmapAdd(stackmap->raw, i + generator->frameRawAreaSize);
	}

	ordCollAddObject(generator->stackmaps, (Object *) stackmap);
	closeHandleScope(&scope, NULL);
}


void generateCCall(CodeGenerator *generator, intptr_t cFunction, size_t argsSize, _Bool storeIp)
{
	AssemblerBuffer *buffer = &generator->buffer;

	if (storeIp) {
		asmLeaq(buffer, asmMem(RIP, NO_REGISTER, SS_1, 0), TMP);
		generateStackmap(generator);
		asmPushq(buffer, TMP);
	}

	asmPushq(buffer, RBP);
	asmMovq(buffer, RSP, RBP);
	asmPushq(buffer, CTX); // spill current context
	asmAndqImm(buffer, RSP, ~(16 - 1)); // ensure 16 bytes stack aligment

	// load thread
	asmMovqMem(buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, thread)), TMP);
	// load last entry frame
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, offsetof(Thread, stackFramesTail)), TMP);
	// set exit frame
	asmMovqToMem(buffer, RBP, asmMem(TMP, NO_REGISTER, SS_1, offsetof(EntryStackFrame, exit)));

	asmMovqImm(buffer, (uint64_t) cFunction, TMP);
	asmCallq(buffer, TMP);

	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -sizeof(intptr_t)), CTX); // restore context
	asmMovq(buffer, RBP, RSP); // restore maybe incorrectly aligned RSP
	asmPopq(buffer, RBP);
	if (storeIp) {
		asmAddqImm(buffer, RSP, 8);
	}
}


void generateMethodContextAllocation(CodeGenerator *generator, size_t size)
{
	AssemblerBuffer *buffer = &generator->buffer;
	Register reg = CTX;
	ptrdiff_t frameOffset = -2 * sizeof(intptr_t);
	ptrdiff_t compiledCodeOffset = offsetof(NativeCode, compiledCode) - offsetof(NativeCode, insts);

	// spill parent context
	asmMovqToMem(buffer, reg, asmMem(RBP, NO_REGISTER, SS_1, frameOffset));

	// allocate new context
	generateLoadObject(buffer, (RawObject *) Handles.MethodContext->raw, RSI, 0);
	if (size == 0) {
		asmXorq(buffer, RDX, RDX);
	} else {
		asmMovqImm(buffer, size, RDX);
	}
	generateStubCall(generator, &AllocateStub);

	// setup RBP
	asmMovqToMem(buffer, RBP, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawContext, frame)));
	// setup parent context
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, frameOffset), reg);
	generateStoreCheck(generator, RAX, reg);
	asmMovqToMem(buffer, reg, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawContext, parent)));
	// load thread
	asmMovqMem(buffer, asmMem(reg, NO_REGISTER, SS_1, varOffset(RawContext, thread)), reg);
	// setup thread
	asmMovqToMem(buffer, reg, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawContext, thread)));
	// load native code entry
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -sizeof(intptr_t)), reg);
	// load compiled code
	asmMovqMem(buffer, asmMem(reg, NO_REGISTER, SS_1, compiledCodeOffset), reg);
	// tag compiled code
	asmIncq(buffer, reg);
	// setup compiled code within new context
	generateStoreCheck(generator, RAX, reg);
	asmMovqToMem(buffer, reg, asmMem(RAX, NO_REGISTER, SS_1, varOffset(RawContext, code)));
	// move context to designated context register
	asmMovq(buffer, RAX, reg);
	// spill context
	asmMovqToMem(buffer, reg, asmMem(RBP, NO_REGISTER, SS_1, frameOffset));
}


void generateBlockContextAllocation(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	ptrdiff_t ctxSizeOffset = offsetof(RawCompiledBlock, header) + offsetof(CompiledCodeHeader, contextSize) - 1;

	// allocate context
	generateLoadObject(buffer, (RawObject *) Handles.BlockContext->raw, RSI, 0);
	asmMovzxbMemq(buffer, asmMem(TMP, NO_REGISTER, SS_1, ctxSizeOffset), RDX);
	generateStubCall(generator, &AllocateStub);
	asmMovq(buffer, RAX, CTX);

	// load parent context
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, -2 * sizeof(intptr_t)), TMP);
	// setup parent context
	asmMovqToMem(buffer, TMP, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, parent)));
	// setup thread
	asmMovqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, varOffset(RawContext, thread)), TMP);
	asmMovqToMem(buffer, TMP, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, thread)));
	// load block
	asmMovqMem(buffer, asmMem(RBP, NO_REGISTER, SS_1, 2 * sizeof(intptr_t)), RDI);
	// move home context from block to new context
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, varOffset(RawBlock, homeContext)), TMP);
	asmMovqToMem(buffer, TMP, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, home)));
	// move compiled block from block to new context
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, varOffset(RawBlock, compiledBlock)), TMP);
	asmMovqToMem(buffer, TMP, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, code)));
	// move outerContext from block to new context
	asmMovqMem(buffer, asmMem(RDI, NO_REGISTER, SS_1, varOffset(RawBlock, outerContext)), TMP);
	asmMovqToMem(buffer, TMP, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, outer)));
}


void generatePushDummyContext(AssemblerBuffer *buffer)
{
	// load thread
	asmMovqMem(buffer, asmMem(CTX, NO_REGISTER, SS_1, varOffset(RawContext, thread)), TMP);
	// push dummy context
	asmPushqMem(buffer, asmMem(TMP, NO_REGISTER, SS_1, offsetof(Thread, context)));
}


NativeCode *generateDoesNotUnderstand(String *selector)
{
	size_t argsSize = computeArguments(selector);
	AssemblerBuffer buffer;
	asmInitBuffer(&buffer, 64);

	generateLoadObject(&buffer, (RawObject *) selector->raw, RDI, 1);
	asmMovqImm(&buffer, argsSize, RDX);

	// jump to stub
	asmMovqImm(&buffer, (uint64_t) getStubNativeCode(&DoesNotUnderstandStub)->insts, R11);
	asmJmpq(&buffer, R11);

	return buildNativeCodeFromAssembler(&buffer);
}


NativeCode *buildNativeCode(CodeGenerator *generator)
{
	NativeCode *code = buildNativeCodeFromAssembler(&generator->buffer);
	if (generator->code.methodOrBlock != NULL) {
		code->compiledCode = ((Object *) generator->code.methodOrBlock)->raw;
		code->argsSize = generator->code.header.argsSize;
	}
	if (generator->descriptors != NULL) {
		code->descriptors = ordCollAsArray(generator->descriptors)->raw;
	}
	if (generator->stackmaps != NULL) {
		code->stackmaps = ordCollAsArray(generator->stackmaps)->raw;
	}
	return code;
}


NativeCode *buildNativeCodeFromAssembler(AssemblerBuffer *buffer)
{
	size_t size = asmOffset(buffer);
	NativeCode *code = allocateNativeCode(&CurrentThread.heap, size, buffer->pointersOffsetsSize);
	code->compiledCode = NULL;
	code->argsSize = 0;
	code->descriptors = NULL;
	code->stackmaps = NULL;
	code->typeFeedback = NULL;
	code->counter = 0;
	asmBindFixups(buffer, code->insts);
	asmCopyBuffer(buffer, code->insts, size);
	asmCopyPointersOffsets(buffer, (uint16_t *) (code->insts + size));
	return code;
}
