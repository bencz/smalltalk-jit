// Tier-1 speculative inliner: rewrites a hot method's BYTECODES, guided by
// the IC cells of its superseded NativeCode, so the ordinary code generator
// (register allocation, stackmaps, descriptors, the M1 direct-call promotion)
// compiles the inlined form with zero backend-specific work.
//
// Shape of an inlined site (see Optimizer.h): an exact-class guard on the
// ORIGINAL receiver operand jumps to the untouched original send on any other
// class, so the floor is exactly the tier-0 send; on the hit, the receiver
// and every argument are spilled into fresh caller temps (single-evaluation
// semantics, and the spilled receiver temp is the instance every rewritten
// ivar access hangs off) and the callee body runs inline.
//
// Eligible callees are strictly LEAF and STRAIGHT-LINE: no primitive, no
// context, no outer returns, no blocks, no jumps, no super/thisContext, no
// writes to their own parameters, a RETURN only in tail position, and at most
// tierInlineMax() bytecode bytes. One level only: bodies are emitted as plain
// sends, never re-inlined.
//
// Correspondence contract (the load-bearing invariant): the i-th DYNAMIC send
// of the original bytecodes pairs with old IC cell i, computed here with the
// SAME classification the backends use (jit/SendClassify.h: dynamic receiver,
// not an identity selector). A cell is consumed by the guard of an inlined
// site or forwarded through the site map so the codegen can still promote the
// copied send to a direct call. Cells are read AT DECISION TIME with no
// allocation between the state load and the class handle (no safepoint, so
// the STW sweep cannot free the state under us); everything after that uses
// the handle.
#include "compiler/Optimizer.h"
#include "compiler/Bytecodes.h"
#include "compiler/Compiler.h"
#include "core/Class.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "core/Assert.h"
#include "jit/CodeDescriptors.h"
#include "jit/InlineCache.h"
#include "jit/SendClassify.h"
#include "jit/Tier.h"
#include <stdlib.h>
#include <string.h>

#define OPT_MAX_ARGS 15
// Merged literal frame budget: literal indexes are one byte, and a site is
// only attempted when the worst case (every callee literal fresh + the guard
// class) still fits.
#define OPT_MAX_LITERALS 240
// Merged temp budget: computeRegsAlloc asserts tempsSize <= 128 and every var
// index must stay a byte with head-room for the specialVars appended after.
#define OPT_MAX_TEMPS 120

typedef struct {
	AssemblerBuffer buffer;
	CompiledCode code;               // the ORIGINAL method, bytes pinned
	OrderedCollection *literals;     // merged frame; caller indexes preserved
	OrderedCollection *descriptors;  // new SOURCE descriptors, one per instruction
	Array *callerDescriptors;        // original source descriptors (may be nil)
	size_t callerTempsEnd;           // 2 + argsSize + tempsSize = inline area base
	size_t inlineAreaSize;           // max over sites of 1 + calleeArgs + calleeTemps
	size_t inlinedSites;
	IcCell **siteMap;
	size_t siteMapCap;
	size_t notedInstructions;        // descriptors emitted so far
	uint16_t curLine;
	uint16_t curColumn;
} Optimizer;

typedef struct {
	ptrdiff_t origTarget;            // original byte offset the jump aims at
	_Bool bound;
	AssemblerLabel label;
} JumpReloc;

typedef struct {
	Optimizer *opt;
	Array *calleeLiterals;
	InstanceShape shape;             // the GUARD class's shape (exact receiver)
	size_t base;                     // spilled-self temp index
	uint8_t calleeArgs;
} InlineContext;

static void setSourcePos(Optimizer *opt, ptrdiff_t origInstruction);
static void noteInstructions(Optimizer *opt);
static void emitRawInstruction(Optimizer *opt, uint8_t *start, size_t length);
static void mapSet(Optimizer *opt, size_t instruction, IcCell *cell);
static _Bool tryInlineSite(Optimizer *opt, IcCell *cell, uint8_t selectorIndex,
	uint8_t argsSize, Operand receiver, Operand *streamArgs, Operand result,
	_Bool withStore);
static _Bool inlineEligible(CompiledCode *callee, InstanceShape shape);
static _Bool operandEligible(Operand operand, InstanceShape shape);
static void emitInlinedBody(InlineContext *ctx, CompiledCode *callee, Operand result);
static void adjustOperand(InlineContext *ctx, Operand *operand);


CompiledMethod *optimizeMethod(CompiledMethod *method, NativeCode *oldCode,
	IcCell ***siteMapOut, size_t *siteMapSizeOut)
{
	*siteMapOut = NULL;
	*siteMapSizeOut = 0;
	if (tierInlineMax() == 0) {
		return NULL;
	}

	HandleScope scope;
	openHandleScope(&scope);

	Optimizer opt;
	memset(&opt, 0, sizeof(opt));
	initMethodCompiledCode(&opt.code, method);
	pinCompiledCodeBytes(&opt.code); // the method object moves; the walk must not
	opt.literals = arrayAsOrdColl(compiledMethodGetLiterals(method));
	opt.descriptors = newOrdColl(32);
	opt.callerDescriptors = compiledMethodGetDescriptors(method);
	opt.callerTempsEnd = 2 + opt.code.header.argsSize + opt.code.header.tempsSize;
	asmInitBuffer(&opt.buffer, 256);

	size_t bcSize = opt.code.bytecodesSize;
	// New byte offset of every original instruction start (backward-jump binds).
	ptrdiff_t *newOffsetAt = malloc((bcSize + 1) * sizeof(ptrdiff_t));
	// One reloc per original jump; a jump is 5+ bytes, so bcSize bounds them.
	JumpReloc *relocs = malloc((bcSize + 1) * sizeof(JumpReloc));
	size_t relocCount = 0;

	IcCell *oldCells = nativeCodeIcCells(oldCode);
	size_t oldCellCount = oldCode->icCellsSize;
	size_t oldSiteIndex = 0;

	BytecodesIterator iterator;
	bytecodeInitIterator(&iterator, opt.code.bytecodes, bcSize);
	while (bytecodeHasNext(&iterator)) {
		ptrdiff_t origOffset = bytecodeOffset(&iterator);
		newOffsetAt[origOffset] = asmOffset(&opt.buffer);
		for (size_t r = 0; r < relocCount; r++) {
			if (!relocs[r].bound && relocs[r].origTarget == origOffset) {
				asmLabelBind(&opt.buffer, &relocs[r].label, asmOffset(&opt.buffer));
				relocs[r].bound = 1;
			}
		}

		Bytecode bytecode = bytecodeNext(&iterator);
		setSourcePos(&opt, bytecodeNumber(&iterator));

		switch (bytecode) {
		case BYTECODE_COPY:
			bytecodeNextOperand(&iterator);
			bytecodeNextOperand(&iterator);
			emitRawInstruction(&opt, opt.code.bytecodes + origOffset,
				bytecodeOffset(&iterator) - origOffset);
			break;

		case BYTECODE_SEND:
		case BYTECODE_SEND_WITH_STORE: {
			uint8_t selectorIndex = bytecodeNextByte(&iterator);
			uint8_t argsSize = bytecodeNextByte(&iterator);
			Operand receiver = bytecodeNextOperand(&iterator);
			Operand streamArgs[OPT_MAX_ARGS];
			_Bool argsFit = argsSize <= OPT_MAX_ARGS;
			for (uint8_t i = 0; i < argsSize; i++) {
				Operand arg = bytecodeNextOperand(&iterator);
				if (argsFit) {
					streamArgs[i] = arg;
				}
			}
			Operand result = { .isValid = 0 };
			if (bytecode == BYTECODE_SEND_WITH_STORE) {
				result = bytecodeNextOperand(&iterator);
			}

			// The backends' site classification, verbatim (SendClassify.h).
			RawObject *selector = compiledCodeLiteralAt(&opt.code, selectorIndex);
			_Bool dynamic = compiledCodeResolveOperandClass(&opt.code, receiver) == NULL
				&& classifyIdentity(selector, argsSize) == IDENT_NONE;
			IcCell *cell = NULL;
			if (dynamic && oldSiteIndex < oldCellCount) {
				cell = &oldCells[oldSiteIndex];
			}
			if (dynamic) {
				oldSiteIndex++;
			}

			if (cell != NULL && argsFit && tryInlineSite(&opt, cell, selectorIndex,
					argsSize, receiver, streamArgs, result,
					bytecode == BYTECODE_SEND_WITH_STORE)) {
				break;
			}
			// Not inlined: the send passes through byte-for-byte, carrying its
			// cell so the codegen's M1 promotion still sees the feedback.
			emitRawInstruction(&opt, opt.code.bytecodes + origOffset,
				bytecodeOffset(&iterator) - origOffset);
			if (cell != NULL) {
				mapSet(&opt, (size_t) opt.buffer.instOffset - 1, cell);
			}
			break;
		}

		case BYTECODE_RETURN:
		case BYTECODE_OUTER_RETURN:
			bytecodeNextOperand(&iterator);
			emitRawInstruction(&opt, opt.code.bytecodes + origOffset,
				bytecodeOffset(&iterator) - origOffset);
			break;

		case BYTECODE_JUMP: {
			// Inlined bodies change instruction sizes, so every original jump
			// is re-emitted through a label keyed by its ORIGINAL target
			// offset: forward ones bind when the walk reaches the target,
			// backward ones bind now from newOffsetAt.
			int32_t disp = bytecodeNextInt32(&iterator);
			ptrdiff_t target = bytecodeOffset(&iterator) + disp;
			JumpReloc *reloc = &relocs[relocCount++];
			reloc->origTarget = target;
			reloc->bound = 0;
			asmInitLabel(&reloc->label);
			if (disp < 0) {
				asmLabelBind(&opt.buffer, &reloc->label, newOffsetAt[target]);
				reloc->bound = 1;
			}
			bytecodeJump(&opt.buffer, &reloc->label);
			noteInstructions(&opt);
			break;
		}

		case BYTECODE_JUMP_NOT_MEMBER_OF: {
			uint8_t classIndex = bytecodeNextByte(&iterator);
			Operand operand = bytecodeNextOperand(&iterator);
			int32_t disp = bytecodeNextInt32(&iterator);
			ptrdiff_t target = bytecodeOffset(&iterator) + disp;
			JumpReloc *reloc = &relocs[relocCount++];
			reloc->origTarget = target;
			reloc->bound = 0;
			asmInitLabel(&reloc->label);
			if (disp < 0) {
				asmLabelBind(&opt.buffer, &reloc->label, newOffsetAt[target]);
				reloc->bound = 1;
			}
			bytecodeJumpNotMemberOf(&opt.buffer, &operand, classIndex, &reloc->label);
			noteInstructions(&opt);
			break;
		}

		default:
			FAIL();
		}
	}

	// Jumps aiming one past the last instruction bind at the new end.
	for (size_t r = 0; r < relocCount; r++) {
		if (!relocs[r].bound && relocs[r].origTarget == (ptrdiff_t) bcSize) {
			asmLabelBind(&opt.buffer, &relocs[r].label, asmOffset(&opt.buffer));
			relocs[r].bound = 1;
		}
		ASSERT(relocs[r].bound);
	}
	free(newOffsetAt);
	free(relocs);

	if (opt.inlinedSites == 0) {
		free(opt.siteMap);
		unpinCompiledCodeBytes(&opt.code);
		asmFreeBuffer(&opt.buffer);
		closeHandleScope(&scope, NULL);
		return NULL;
	}

	// The fresh method: same identity (selector/owner/source) so backtraces
	// and tooling read it as the original, merged literals, the widened temp
	// area, and source descriptors rebuilt per NEW instruction number.
	size_t newSize = asmOffset(&opt.buffer);
	size_t instructionCount = (size_t) opt.buffer.instOffset;
	CompiledMethod *newMethod = newObject(Handles.CompiledMethod, newSize);
	asmCopyBuffer(&opt.buffer, compiledMethodGetBytes(newMethod), newSize);
	CompiledCodeHeader header = opt.code.header;
	ASSERT(opt.code.header.tempsSize + opt.inlineAreaSize <= OPT_MAX_TEMPS);
	header.tempsSize = (uint8_t) (opt.code.header.tempsSize + opt.inlineAreaSize);
	compiledMethodSetHeader(newMethod, header);
	compiledMethodSetLiterals(newMethod, ordCollAsArray(opt.literals));
	compiledMethodSetSelector(newMethod, compiledMethodGetSelector(method));
	compiledMethodSetOwnerClass(newMethod, compiledMethodGetOwnerClass(method));
	compiledMethodSetSourceCode(newMethod, compiledMethodGetSourceCode(method));
	compiledMethodSetDescriptors(newMethod, ordCollAsArray(opt.descriptors));

	// Hand over a map covering every instruction (missing tail entries NULL).
	if (opt.siteMapCap < instructionCount) {
		opt.siteMap = realloc(opt.siteMap, instructionCount * sizeof(IcCell *));
		memset(opt.siteMap + opt.siteMapCap, 0,
			(instructionCount - opt.siteMapCap) * sizeof(IcCell *));
	}
	*siteMapOut = opt.siteMap;
	*siteMapSizeOut = instructionCount;

	unpinCompiledCodeBytes(&opt.code);
	asmFreeBuffer(&opt.buffer);
	return closeHandleScope(&scope, newMethod);
}


// Source position (line/column) of the original instruction, carried onto
// every new instruction emitted for it, inlined body included: an exception
// inside an inlined callee attributes to the CALL SITE of the caller frame,
// the only frame that exists.
static void setSourcePos(Optimizer *opt, ptrdiff_t origInstruction)
{
	Value descriptor = 0;
	if (!isNil((Object *) opt->callerDescriptors)) {
		descriptor = descriptorsAtPosition(opt->callerDescriptors->raw,
			(uint16_t) origInstruction);
	}
	opt->curLine = descriptorGetLine(descriptor);
	opt->curColumn = descriptorGetColumn(descriptor);
}


static void noteInstructions(Optimizer *opt)
{
	while (opt->notedInstructions < (size_t) opt->buffer.instOffset) {
		ordCollAdd(opt->descriptors, createSouceCodeDescriptor(
			(uint16_t) opt->notedInstructions, opt->curLine, opt->curColumn));
		opt->notedInstructions++;
	}
}


// Verbatim pass-through of one original instruction (COPY/SEND/RETURN carry
// no stream-relative references, so their bytes are position-independent).
static void emitRawInstruction(Optimizer *opt, uint8_t *start, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		asmEnsureCapacity(&opt->buffer);
		asmEmitUint8(&opt->buffer, start[i]);
	}
	opt->buffer.instOffset++;
	noteInstructions(opt);
}


static void mapSet(Optimizer *opt, size_t instruction, IcCell *cell)
{
	if (instruction >= opt->siteMapCap) {
		size_t cap = opt->siteMapCap == 0 ? 64 : opt->siteMapCap;
		while (cap <= instruction) {
			cap *= 2;
		}
		opt->siteMap = realloc(opt->siteMap, cap * sizeof(IcCell *));
		memset(opt->siteMap + opt->siteMapCap, 0,
			(cap - opt->siteMapCap) * sizeof(IcCell *));
		opt->siteMapCap = cap;
	}
	opt->siteMap[instruction] = cell;
}


static _Bool tryInlineSite(Optimizer *opt, IcCell *cell, uint8_t selectorIndex,
	uint8_t argsSize, Operand receiver, Operand *streamArgs, Operand result,
	_Bool withStore)
{
	// The guard re-reads the receiver operand, so it must be one of the
	// dynamically-checked forms of generateClassCheck; the result must be a
	// legal copy destination.
	switch (receiver.type) {
	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:
	case OPERAND_CONTEXT_VAR:
	case OPERAND_INST_VAR:
	case OPERAND_ASSOC:
		break;
	default:
		return 0;
	}
	if (result.isValid) {
		switch (result.type) {
		case OPERAND_TEMP_VAR:
		case OPERAND_CONTEXT_VAR:
		case OPERAND_INST_VAR:
		case OPERAND_ASSOC:
			break;
		default:
			return 0;
		}
	}

	// Read the cell once; no allocation before the class is handled (the STW
	// sweep frees states only with every mutator parked).
	IcState *state = __atomic_load_n(&cell->state, __ATOMIC_ACQUIRE);
	if (state->kind != IC_KIND_MONO) {
		return 0;
	}
	Value taggedClass = state->class;

	HandleScope scope;
	openHandleScope(&scope);
	Class *classHandle = scopeHandle((RawClass *) asObject(taggedClass));
	String *selectorHandle = scopeHandle(
		(RawString *) compiledCodeLiteralAt(&opt->code, selectorIndex));
	CompiledMethod *callee = lookupSelector(classHandle, selectorHandle);
	if (callee == NULL) { // a DNU trampoline bind: nothing to inline
		closeHandleScope(&scope, NULL);
		return 0;
	}

	InstanceShape shape = classGetInstanceShape(classHandle);
	CompiledCode calleeCode;
	initMethodCompiledCode(&calleeCode, callee);
	if (calleeCode.header.argsSize != argsSize
			|| !inlineEligible(&calleeCode, shape)
			|| ordCollSize(opt->literals) + calleeCode.bytecodesSize / 2 + 1 > OPT_MAX_LITERALS
			|| opt->callerTempsEnd - 2
				+ 1 + argsSize + calleeCode.header.tempsSize > OPT_MAX_TEMPS) {
		closeHandleScope(&scope, NULL);
		return 0;
	}

	pinCompiledCodeBytes(&calleeCode); // the callee method object moves too

	size_t base = opt->callerTempsEnd;
	ptrdiff_t classLiteral = ordCollAddObjectIfNotExists(opt->literals, (Object *) classHandle);
	ASSERT(classLiteral <= 255);

	AssemblerLabel fallback, done;
	asmInitLabel(&fallback);
	asmInitLabel(&done);

	// Exact-class guard on the ORIGINAL receiver operand; any other class
	// takes the untouched original send below.
	bytecodeJumpNotMemberOf(&opt->buffer, &receiver, (uint8_t) classLiteral, &fallback);
	noteInstructions(opt);

	// Single evaluation: receiver and arguments (stream order is REVERSED
	// source order) spill into fresh temps before the body runs; the body
	// never touches caller state except through these.
	Operand selfTemp = { .isValid = 1, .type = OPERAND_TEMP_VAR, .index = (uint8_t) base };
	bytecodeCopy(&opt->buffer, &receiver, &selfTemp);
	noteInstructions(opt);
	for (uint8_t k = 0; k < argsSize; k++) {
		Operand argTemp = { .isValid = 1, .type = OPERAND_TEMP_VAR,
			.index = (uint8_t) (base + 1 + k) };
		bytecodeCopy(&opt->buffer, &streamArgs[argsSize - 1 - k], &argTemp);
		noteInstructions(opt);
	}

	InlineContext ctx = {
		.opt = opt,
		.calleeLiterals = compiledMethodGetLiterals(callee),
		.shape = shape,
		.base = base,
		.calleeArgs = calleeCode.header.argsSize,
	};
	emitInlinedBody(&ctx, &calleeCode, result);

	bytecodeJump(&opt->buffer, &done);
	noteInstructions(opt);

	asmLabelBind(&opt->buffer, &fallback, asmOffset(&opt->buffer));
	// The fallback is the original send, byte-equal semantics: args back in
	// SOURCE order so bytecodeSend re-reverses them into the original stream.
	{
		Operand sourceArgs[OPT_MAX_ARGS];
		for (uint8_t k = 0; k < argsSize; k++) {
			sourceArgs[k] = streamArgs[argsSize - 1 - k];
		}
		if (withStore) {
			bytecodeSendWithStore(&opt->buffer, selectorIndex, &receiver,
				&result, sourceArgs, argsSize);
		} else {
			bytecodeSend(&opt->buffer, selectorIndex, &receiver, sourceArgs, argsSize);
		}
		noteInstructions(opt);
	}
	asmLabelBind(&opt->buffer, &done, asmOffset(&opt->buffer));

	size_t area = 1 + argsSize + calleeCode.header.tempsSize;
	if (area > opt->inlineAreaSize) {
		opt->inlineAreaSize = area;
	}
	opt->inlinedSites++;
	gTierStats.inlinedSites++;

	unpinCompiledCodeBytes(&calleeCode);
	closeHandleScope(&scope, NULL);
	return 1;
}


_Bool optimizerInlineEligibleForTest(CompiledMethod *callee, Class *receiverClass)
{
	CompiledCode code;
	initMethodCompiledCode(&code, callee);
	return inlineEligible(&code, classGetInstanceShape(receiverClass));
}


// Leaf, straight-line, and rewritable: see the file comment. `shape` is the
// guard class's, used to bound the pre-resolved ivar slot indexes.
static _Bool inlineEligible(CompiledCode *callee, InstanceShape shape)
{
	CompiledCodeHeader header = callee->header;
	if (header.primitive != 0 || header.hasContext || header.outerReturns != 0) {
		return 0;
	}
	if (callee->bytecodesSize == 0 || callee->bytecodesSize > tierInlineMax()) {
		return 0;
	}

	BytecodesIterator iterator;
	bytecodeInitIterator(&iterator, callee->bytecodes, callee->bytecodesSize);
	while (bytecodeHasNext(&iterator)) {
		Bytecode bytecode = bytecodeNext(&iterator);
		switch (bytecode) {
		case BYTECODE_COPY: {
			Operand src = bytecodeNextOperand(&iterator);
			Operand dst = bytecodeNextOperand(&iterator);
			if (!operandEligible(src, shape) || !operandEligible(dst, shape)
					|| dst.type == OPERAND_ARG_VAR) { // parameter writes break substitution
				return 0;
			}
			break;
		}
		case BYTECODE_SEND:
		case BYTECODE_SEND_WITH_STORE: {
			bytecodeNextByte(&iterator);
			uint8_t argsSize = bytecodeNextByte(&iterator);
			if (argsSize > OPT_MAX_ARGS) {
				return 0;
			}
			for (uint8_t i = 0; i < argsSize + 1; i++) { // receiver + args
				if (!operandEligible(bytecodeNextOperand(&iterator), shape)) {
					return 0;
				}
			}
			if (bytecode == BYTECODE_SEND_WITH_STORE) {
				Operand result = bytecodeNextOperand(&iterator);
				if (!operandEligible(result, shape) || result.type == OPERAND_ARG_VAR) {
					return 0;
				}
			}
			break;
		}
		case BYTECODE_RETURN:
			if (!operandEligible(bytecodeNextOperand(&iterator), shape)) {
				return 0;
			}
			if (bytecodeHasNext(&iterator)) { // tail position only
				return 0;
			}
			break;
		default: // OUTER_RETURN, JUMP, JUMP_NOT_MEMBER_OF, or corrupt
			return 0;
		}
	}
	return 1;
}


static _Bool operandEligible(Operand operand, InstanceShape shape)
{
	switch (operand.type) {
	case OPERAND_VALUE:
	case OPERAND_NIL:
	case OPERAND_TRUE:
	case OPERAND_FALSE:
	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:
	case OPERAND_LITERAL:
	case OPERAND_ASSOC:
		return 1;
	case OPERAND_INST_VAR:
		// The rewrite bakes the absolute slot; it must stay a byte.
		return shape.payloadSize + operand.index + shape.isIndexed <= 255;
	default: // SUPER, THIS_CONTEXT, CONTEXT_VAR, BLOCK, INST_VAR_OF
		return 0;
	}
}


static void emitInlinedBody(InlineContext *ctx, CompiledCode *callee, Operand result)
{
	Optimizer *opt = ctx->opt;
	BytecodesIterator iterator;
	_Bool returned = 0;
	bytecodeInitIterator(&iterator, callee->bytecodes, callee->bytecodesSize);
	while (bytecodeHasNext(&iterator)) {
		Bytecode bytecode = bytecodeNext(&iterator);
		switch (bytecode) {
		case BYTECODE_COPY: {
			Operand src = bytecodeNextOperand(&iterator);
			Operand dst = bytecodeNextOperand(&iterator);
			adjustOperand(ctx, &src);
			adjustOperand(ctx, &dst);
			bytecodeCopy(&opt->buffer, &src, &dst);
			noteInstructions(opt);
			break;
		}

		case BYTECODE_SEND:
		case BYTECODE_SEND_WITH_STORE: {
			_Bool withStore = bytecode == BYTECODE_SEND_WITH_STORE;
			uint8_t selectorIndex = bytecodeNextByte(&iterator);
			uint8_t argsSize = bytecodeNextByte(&iterator);
			Operand receiver = bytecodeNextOperand(&iterator);
			adjustOperand(ctx, &receiver);
			// Stream order is reversed source order; collect back into source
			// order so bytecodeSend's re-reversal reproduces the callee's own
			// stream layout.
			Operand sourceArgs[OPT_MAX_ARGS];
			for (uint8_t i = 0; i < argsSize; i++) {
				Operand arg = bytecodeNextOperand(&iterator);
				adjustOperand(ctx, &arg);
				sourceArgs[argsSize - 1 - i] = arg;
			}
			ptrdiff_t selector = ordCollAddObjectIfNotExists(opt->literals,
				arrayObjectAt(ctx->calleeLiterals, selectorIndex));
			ASSERT(selector <= 255);
			if (withStore) {
				Operand dst = bytecodeNextOperand(&iterator);
				adjustOperand(ctx, &dst);
				bytecodeSendWithStore(&opt->buffer, (uint8_t) selector, &receiver,
					&dst, sourceArgs, argsSize);
			} else {
				bytecodeSend(&opt->buffer, (uint8_t) selector, &receiver,
					sourceArgs, argsSize);
			}
			noteInstructions(opt);
			break;
		}

		case BYTECODE_RETURN: {
			// Tail position (eligibility): the callee's answer flows into the
			// original send's result operand; a statement-position send
			// discards it, and the return OPERAND is an effect-free read.
			Operand value = bytecodeNextOperand(&iterator);
			returned = 1;
			if (result.isValid) {
				adjustOperand(ctx, &value);
				bytecodeCopy(&opt->buffer, &value, &result);
				noteInstructions(opt);
			}
			break;
		}

		default:
			FAIL(); // eligibility excluded everything else
		}
	}

	if (!returned && result.isValid) {
		// Fell off the end: a method answers self.
		Operand selfTemp = { .isValid = 1, .type = OPERAND_TEMP_VAR,
			.index = (uint8_t) ctx->base };
		bytecodeCopy(&opt->buffer, &selfTemp, &result);
		noteInstructions(opt);
	}
}


// Remap one callee operand into the caller's frame: self and parameters to
// the spilled temps, callee temps into the inline area, ivars to the
// pre-resolved INST_VAR_OF form off the spilled self, literals re-interned
// into the merged frame. Anything else was rejected by eligibility.
static void adjustOperand(InlineContext *ctx, Operand *operand)
{
	switch (operand->type) {
	case OPERAND_VALUE:
	case OPERAND_NIL:
	case OPERAND_TRUE:
	case OPERAND_FALSE:
		break;

	case OPERAND_TEMP_VAR:
		ASSERT(operand->index >= 2 + ctx->calleeArgs);
		operand->index = (uint8_t) (ctx->base + 1 + ctx->calleeArgs
			+ (operand->index - 2 - ctx->calleeArgs));
		break;

	case OPERAND_ARG_VAR:
		operand->type = OPERAND_TEMP_VAR;
		operand->index = operand->index == SELF_INDEX
			? (uint8_t) ctx->base
			: (uint8_t) (ctx->base + 1 + (operand->index - 2));
		break;

	case OPERAND_INST_VAR:
		operand->instance.type = OPERAND_TEMP_VAR;
		operand->instance.index = (uint8_t) ctx->base;
		operand->instance.level = 0;
		operand->index = (uint8_t) (ctx->shape.payloadSize + operand->index
			+ ctx->shape.isIndexed);
		operand->type = OPERAND_INST_VAR_OF;
		break;

	case OPERAND_LITERAL:
	case OPERAND_ASSOC: {
		ptrdiff_t index = ordCollAddObjectIfNotExists(ctx->opt->literals,
			arrayObjectAt(ctx->calleeLiterals, operand->index));
		ASSERT(index <= 255);
		operand->index = (uint8_t) index;
		break;
	}

	default:
		FAIL();
	}
}
