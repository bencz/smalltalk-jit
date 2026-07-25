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
// Merged temp budget: CompiledCodeHeader.tempsSize is a uint8, so the merged
// caller + inline-area count must stay under 256 (the register allocator itself
// is sized per method now). Conservative head-room kept from the old limit.
#define OPT_MAX_TEMPS 120
// Independent ceiling on a callee's bytecode size, bounding the fixed-size
// instruction-start and jump-target maps in inlineEligible. ST_TIER_INLINE_MAX
// is settable at runtime, so this must not assume anything about it.
#define OPT_MAX_CALLEE_BYTES 512

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
static _Bool tryInlineSite(Optimizer *opt, IcCell *cell, uint16_t selectorIndex,
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
	if (typeStatsEnabled()) {
		gTypeStats.hotMethods++;
	}

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
			uint16_t selectorIndex = bytecodeNextUint16(&iterator);
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
			_Bool identity = classifyIdentity(selector, argsSize) != IDENT_NONE;
			_Bool resolved = compiledCodeResolveOperandClass(&opt.code, receiver) != NULL;
			_Bool dynamic = !resolved && !identity;
			IcCell *cell = NULL;
			if (dynamic && oldSiteIndex < oldCellCount) {
				cell = &oldCells[oldSiteIndex];
			}
			if (dynamic) {
				oldSiteIndex++;
			}
			// ST_TYPE_STATS, hot scope: this method reached tier 1, and the cell
			// is the site's accumulated feedback. Counted BEFORE tryInlineSite
			// so an inlined site still appears, with the mono verdict that is
			// exactly why it got inlined.
			typeStatsNoteSend(1, receiver, identity, resolved, cell);

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
			uint16_t classIndex = bytecodeNextUint16(&iterator);
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


static _Bool tryInlineSite(Optimizer *opt, IcCell *cell, uint16_t selectorIndex,
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
	//
	// Mark the guard's instruction number so the backends can register it as a
	// SPEC_GUARD (jit/SpecSite.h): the inlined body below is a COPY of the
	// callee's bytecodes, and redefining the callee's class leaves that copy
	// stale while the guard still matches (redefineMutate preserves the class
	// object identity). The compiler emits the same bytecode for inlined
	// boolean control flow, which is NOT speculative, hence the marker.
	mapSet(opt, opt->buffer.instOffset, &gTierSpecGuard);
	bytecodeJumpNotMemberOf(&opt->buffer, &receiver, (uint16_t) classLiteral, &fallback);
	noteInstructions(opt);

	// Single evaluation: receiver and arguments (stream order is REVERSED
	// source order) spill into fresh temps before the body runs; the body
	// never touches caller state except through these.
	Operand selfTemp = { .isValid = 1, .type = OPERAND_TEMP_VAR, .index = (uint16_t) base };
	bytecodeCopy(&opt->buffer, &receiver, &selfTemp);
	noteInstructions(opt);
	for (uint8_t k = 0; k < argsSize; k++) {
		Operand argTemp = { .isValid = 1, .type = OPERAND_TEMP_VAR,
			.index = (uint16_t) (base + 1 + k) };
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
	// Bound for the fixed-size maps below. tierInlineMax is settable, so this
	// is a second, independent ceiling rather than an assumption about it.
	if (callee->bytecodesSize > OPT_MAX_CALLEE_BYTES) {
		return 0;
	}

	// Every byte offset that starts an instruction, plus every jump target, so
	// a target can be PROVED to land on a real instruction boundary inside this
	// callee. Nothing else validates that: a target that split an instruction
	// would have the body emit a jump into the middle of one, and the resulting
	// bytecode would decode as garbage rather than fail.
	_Bool isStart[OPT_MAX_CALLEE_BYTES + 1];
	ptrdiff_t targets[OPT_MAX_CALLEE_BYTES + 1];
	size_t targetCount = 0;
	memset(isStart, 0, callee->bytecodesSize + 1);
	isStart[callee->bytecodesSize] = 1; // one past the end: falls off the body

	BytecodesIterator iterator;
	bytecodeInitIterator(&iterator, callee->bytecodes, callee->bytecodesSize);
	while (bytecodeHasNext(&iterator)) {
		isStart[bytecodeOffset(&iterator)] = 1;
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
			bytecodeNextUint16(&iterator);
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
			// Any position, not just the tail: emitInlinedBody gives each
			// return its own exit jump to the body's merge point.
			if (!operandEligible(bytecodeNextOperand(&iterator), shape)) {
				return 0;
			}
			break;

		case BYTECODE_JUMP: {
			int32_t disp = bytecodeNextInt32(&iterator);
			targets[targetCount++] = bytecodeOffset(&iterator) + disp;
			break;
		}

		case BYTECODE_JUMP_NOT_MEMBER_OF: {
			bytecodeNextUint16(&iterator); // class literal, re-interned on emit
			Operand guarded = bytecodeNextOperand(&iterator);
			if (!operandEligible(guarded, shape)) {
				return 0;
			}
			// A guarded INST_VAR is remapped by adjustOperand into
			// OPERAND_INST_VAR_OF. That form used to have no arm in
			// generateClassCheck on either backend, so a callee branching on one
			// of its own instance variables had to be rejected here (found by the
			// ST_TIER_INLINE_MAX sweep, which crashed Richards at 128). Both
			// backends now handle it, so every eligible operand maps to a form
			// generateClassCheck knows. tests/InlineControlFlowTest.st pins it.
			int32_t disp = bytecodeNextInt32(&iterator);
			targets[targetCount++] = bytecodeOffset(&iterator) + disp;
			break;
		}

		default: // OUTER_RETURN (needs a real context) or corrupt
			return 0;
		}
	}

	for (size_t i = 0; i < targetCount; i++) {
		if (targets[i] < 0 || targets[i] > (ptrdiff_t) callee->bytecodesSize
				|| !isStart[targets[i]]) {
			return 0;
		}
	}
	return 1;
}


// Can control reach the END of the callee body, either by falling out of the
// last instruction or by a jump aimed one past it? Used to decide whether the
// "a method answers self" copy is emitted at all, and therefore whether a
// tail RETURN needs a jump to skip it. Keeping this exact is what makes a
// straight-line callee emit byte-identical bytecode to before control flow
// was allowed, so the existing inlining does not regress.
static _Bool calleeCanFallThrough(CompiledCode *callee)
{
	BytecodesIterator iterator;
	Bytecode last = BYTECODE_COPY;
	bytecodeInitIterator(&iterator, callee->bytecodes, callee->bytecodesSize);
	while (bytecodeHasNext(&iterator)) {
		Bytecode bytecode = bytecodeNext(&iterator);
		last = bytecode;
		switch (bytecode) {
		case BYTECODE_JUMP: {
			int32_t disp = bytecodeNextInt32(&iterator);
			if (bytecodeOffset(&iterator) + disp == (ptrdiff_t) callee->bytecodesSize) {
				return 1;
			}
			break;
		}
		case BYTECODE_JUMP_NOT_MEMBER_OF: {
			bytecodeNextUint16(&iterator);
			bytecodeNextOperand(&iterator);
			int32_t disp = bytecodeNextInt32(&iterator);
			if (bytecodeOffset(&iterator) + disp == (ptrdiff_t) callee->bytecodesSize) {
				return 1;
			}
			break;
		}
		case BYTECODE_COPY:
			bytecodeNextOperand(&iterator);
			bytecodeNextOperand(&iterator);
			break;
		case BYTECODE_SEND:
		case BYTECODE_SEND_WITH_STORE: {
			bytecodeNextUint16(&iterator);
			uint8_t argsSize = bytecodeNextByte(&iterator);
			for (uint8_t i = 0; i < argsSize + 1; i++) {
				bytecodeNextOperand(&iterator);
			}
			if (bytecode == BYTECODE_SEND_WITH_STORE) {
				bytecodeNextOperand(&iterator);
			}
			break;
		}
		default: // RETURN
			bytecodeNextOperand(&iterator);
			break;
		}
	}
	// An unconditional JUMP already answered above if it aimed at the end; any
	// other trailing instruction except a RETURN falls out of the body.
	return last != BYTECODE_RETURN && last != BYTECODE_JUMP;
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


// Emit the callee's body into the caller's stream, remapping every operand and
// RELOCATING every jump.
//
// The callee's jumps get their OWN relocation table and their own
// newOffsetAt map, never the caller's. Both index spaces are plain byte
// offsets and would collide numerically: a callee offset looked up in the
// caller's map silently yields some unrelated caller instruction, which is a
// wrong jump rather than a crash.
//
// Control leaves the body at two distinct points:
//   fallEnd  reached by falling out of the last instruction, or by a jump
//            aimed one past it. This is where "a method answers self" is
//            emitted, because reaching the end without returning IS that case.
//   bodyEnd  reached by an explicit RETURN, which must therefore SKIP the
//            self copy rather than let it clobber the returned value.
// When the self copy is not emitted at all the two coincide.
static void emitInlinedBody(InlineContext *ctx, CompiledCode *callee, Operand result)
{
	Optimizer *opt = ctx->opt;
	BytecodesIterator iterator;
	size_t bcSize = callee->bytecodesSize;

	ptrdiff_t *newOffsetAt = malloc((bcSize + 1) * sizeof(ptrdiff_t));
	JumpReloc *relocs = malloc((bcSize + 1) * sizeof(JumpReloc));
	// One exit label per RETURN; a RETURN is at least 2 bytes, so bcSize bounds
	// them. Each label takes exactly ONE forward reference (asmEmitLabel32
	// asserts it), which is why they cannot share a single `bodyEnd` label.
	AssemblerLabel *returnExits = malloc((bcSize + 1) * sizeof(AssemblerLabel));
	size_t relocCount = 0;
	size_t returnExitCount = 0;
	if (newOffsetAt == NULL || relocs == NULL || returnExits == NULL) {
		FAIL();
	}

	// Emitting the self copy is what forces a tail RETURN to jump over it.
	// Deciding it up front keeps a straight-line callee byte-identical to what
	// the pre-control-flow inliner produced.
	_Bool selfCopy = result.isValid && calleeCanFallThrough(callee);

	bytecodeInitIterator(&iterator, callee->bytecodes, bcSize);
	while (bytecodeHasNext(&iterator)) {
		ptrdiff_t origOffset = bytecodeOffset(&iterator);
		newOffsetAt[origOffset] = asmOffset(&opt->buffer);
		for (size_t r = 0; r < relocCount; r++) {
			if (!relocs[r].bound && relocs[r].origTarget == origOffset) {
				asmLabelBind(&opt->buffer, &relocs[r].label, asmOffset(&opt->buffer));
				relocs[r].bound = 1;
			}
		}

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
			uint16_t selectorIndex = bytecodeNextUint16(&iterator);
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
				bytecodeSendWithStore(&opt->buffer, (uint16_t) selector, &receiver,
					&dst, sourceArgs, argsSize);
			} else {
				bytecodeSend(&opt->buffer, (uint16_t) selector, &receiver,
					sourceArgs, argsSize);
			}
			noteInstructions(opt);
			break;
		}

		case BYTECODE_RETURN: {
			// Any position now. The callee's answer flows into the original
			// send's result operand; a statement-position send discards it,
			// and the return OPERAND is an effect-free read.
			Operand value = bytecodeNextOperand(&iterator);
			if (result.isValid) {
				adjustOperand(ctx, &value);
				bytecodeCopy(&opt->buffer, &value, &result);
				noteInstructions(opt);
			}
			// Skip the rest of the body. Not needed when this is the last
			// instruction AND nothing follows it to skip, which is exactly the
			// straight-line shape the old inliner handled.
			if (bytecodeHasNext(&iterator) || selfCopy) {
				AssemblerLabel *exit = &returnExits[returnExitCount++];
				asmInitLabel(exit);
				bytecodeJump(&opt->buffer, exit);
				noteInstructions(opt);
			}
			break;
		}

		case BYTECODE_JUMP: {
			int32_t disp = bytecodeNextInt32(&iterator);
			ptrdiff_t target = bytecodeOffset(&iterator) + disp;
			JumpReloc *reloc = &relocs[relocCount++];
			reloc->origTarget = target;
			reloc->bound = 0;
			asmInitLabel(&reloc->label);
			if (disp < 0) {
				asmLabelBind(&opt->buffer, &reloc->label, newOffsetAt[target]);
				reloc->bound = 1;
			}
			bytecodeJump(&opt->buffer, &reloc->label);
			noteInstructions(opt);
			break;
		}

		case BYTECODE_JUMP_NOT_MEMBER_OF: {
			uint16_t classIndex = bytecodeNextUint16(&iterator);
			Operand operand = bytecodeNextOperand(&iterator);
			adjustOperand(ctx, &operand);
			int32_t disp = bytecodeNextInt32(&iterator);
			ptrdiff_t target = bytecodeOffset(&iterator) + disp;
			// The guarded class is a CALLEE literal and must be re-interned
			// into the merged frame like every other literal the body uses.
			ptrdiff_t classLiteral = ordCollAddObjectIfNotExists(opt->literals,
				arrayObjectAt(ctx->calleeLiterals, classIndex));
			ASSERT(classLiteral <= 255);
			JumpReloc *reloc = &relocs[relocCount++];
			reloc->origTarget = target;
			reloc->bound = 0;
			asmInitLabel(&reloc->label);
			if (disp < 0) {
				asmLabelBind(&opt->buffer, &reloc->label, newOffsetAt[target]);
				reloc->bound = 1;
			}
			bytecodeJumpNotMemberOf(&opt->buffer, &operand, (uint16_t) classLiteral,
				&reloc->label);
			noteInstructions(opt);
			break;
		}

		default:
			FAIL(); // eligibility excluded everything else
		}
	}

	// fallEnd: jumps aimed one past the last instruction land with the
	// fall-through, because both mean "reached the end without returning".
	for (size_t r = 0; r < relocCount; r++) {
		if (!relocs[r].bound && relocs[r].origTarget == (ptrdiff_t) bcSize) {
			asmLabelBind(&opt->buffer, &relocs[r].label, asmOffset(&opt->buffer));
			relocs[r].bound = 1;
		}
		ASSERT(relocs[r].bound);
	}

	if (selfCopy) {
		// Fell off the end: a method answers self.
		Operand selfTemp = { .isValid = 1, .type = OPERAND_TEMP_VAR,
			.index = (uint16_t) ctx->base };
		bytecodeCopy(&opt->buffer, &selfTemp, &result);
		noteInstructions(opt);
	}

	// bodyEnd: every explicit RETURN, which has already written the result and
	// must not be clobbered by the self copy above.
	for (size_t i = 0; i < returnExitCount; i++) {
		asmLabelBind(&opt->buffer, &returnExits[i], asmOffset(&opt->buffer));
	}

	free(newOffsetAt);
	free(relocs);
	free(returnExits);
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
		operand->index = (uint16_t) (ctx->base + 1 + ctx->calleeArgs
			+ (operand->index - 2 - ctx->calleeArgs));
		break;

	case OPERAND_ARG_VAR:
		operand->type = OPERAND_TEMP_VAR;
		operand->index = operand->index == SELF_INDEX
			? (uint16_t) ctx->base
			: (uint16_t) (ctx->base + 1 + (operand->index - 2));
		break;

	case OPERAND_INST_VAR:
		operand->instance.type = OPERAND_TEMP_VAR;
		operand->instance.index = (uint16_t) ctx->base;
		operand->instance.level = 0;
		operand->index = (uint16_t) (ctx->shape.payloadSize + operand->index
			+ ctx->shape.isIndexed);
		operand->type = OPERAND_INST_VAR_OF;
		break;

	case OPERAND_LITERAL:
	case OPERAND_ASSOC: {
		ptrdiff_t index = ordCollAddObjectIfNotExists(ctx->opt->literals,
			arrayObjectAt(ctx->calleeLiterals, operand->index));
		ASSERT(index <= 255);
		operand->index = (uint16_t) index;
		break;
	}

	default:
		FAIL();
	}
}
