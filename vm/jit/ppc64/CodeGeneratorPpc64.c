// ppc64 backend (BOTH byte orders), the code generator. A mechanical
// translation of vm/jit/x64/CodeGeneratorX64.c under the register/frame
// mapping pinned in vm/jit/ppc64/DESIGN.md (ELFv2 deltas: DESIGN-elfv2.md).
// Read those first; the key invariants are: frame layout byte-compatible
// with the x64 one (StackFrame.c walker untouched),
// FP=r31/CTX=r30/TGT=r12/TMP=r11/TMP2=r10/result=r3, LR discipline (bodies:
// LR dead after the prologue pushed it; generateCCall PUSHES LR before FP so
// [FP+8] is the IC the GC's frame walker reads; framed primitives push LR
// first), instruction-word byte order from the AssemblerPpc64.h selector,
// the C ABI behind the gPpc64Abi vtable, and tagged-base ld/std through
// asmLdT/StdT.
#ifndef __powerpc64__
#error "vm/jit/ppc64/ is powerpc64-only code (both byte orders) - check ST_ARCH in CMakeLists.txt"
#endif

#include <stdio.h>
#include <stdlib.h>
#include "jit/CodeGenerator.h"
#include "os/Os.h"
#include "jit/ppc64/AssemblerPpc64.h"
#include "jit/ppc64/Abi.h"
#include "jit/TargetCodePatch.h"
#include "jit/ppc64/Cpu.h"
#include "core/Object.h"
#include "core/Class.h"
#include "core/Smalltalk.h"
#include "runtime/Primitives.h"
#include "jit/StubCode.h"
#include "memory/Heap.h"
#include "core/StackFrame.h"
#include "core/Handle.h"
#include "compiler/Bytecodes.h"
#include "compiler/Compiler.h"
#include "jit/CodeDescriptors.h"
#include "core/Thread.h"
#include "core/Exception.h"
#include "core/Assert.h"
#include <string.h>

typedef struct {
	ptrdiff_t offset;
	AssemblerLabel label;
} BytecodeLabel;

static NativeCode *generateBlockCode(CompiledBlock *block, CodeGenerator *parentGenerator);
static void generateSafepointPoll(CodeGenerator *generator, _Bool atBackEdge);
static void generateCode(CodeGenerator *generator);
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
static void movToOperand(CodeGenerator *generator, Register reg, Operand operand, _Bool valueMayBePointer);
static _Bool operandMayBePointer(Operand operand);
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


// A Variable whose register is SPILLED_REG (-1) must never reach an emitter as a
// register NUMBER. Spilled variables materialize in r4, one fixed volatile
// scratch used consistently by every such path (fillVar loads into varReg(var),
// and every caller reads varReg(var) back).
//
// This convention is why POWER is correct here while x64 was not: an earlier note
// claimed x64 got away with it because -1 encodes as an accidental volatile
// scratch. That was wrong. On x64 the wild register is a live allocatable one, so
// 5+ nested inlined conditionals (enough to exhaust the pool and start spilling)
// miscompiled into a garbage receiver; see the fillVarToReg staging in
// CodeGeneratorX64.c, which brings x64 up to this same discipline.
static Register varReg(Variable *var)
{
	return var->reg == SPILLED_REG ? R4 : (Register) var->reg;
}


// Load an arbitrary (non-patchable) immediate: one li when it fits, the
// fixed li64 shape otherwise. Patchable object pointers go through
// generateLoadObject instead (always li64 + a registered pointer offset).
static void emitLoadImm(AssemblerBuffer *buffer, Register reg, int64_t value)
{
	if (-32768 <= value && value <= 32767) {
		asmLi(buffer, reg, (ptrdiff_t) value);
	} else {
		asmLi64(buffer, reg, (uint64_t) value);
	}
}


NativeCode *generateMethodCode(CompiledMethod *method)
{
	heapCodegenLockEnter(CurrentThread.heap); // serialize codegen across worker threads
	HandleScope scope;
	openHandleScope(&scope);

	CodeGenerator generator;
	// init FIRST (it resets code.methodOrBlock), then bind the method's code
	initCodeGenerator(&generator);
	initMethodCompiledCode(&generator.code, method);
	pinCompiledCodeBytes(&generator.code); // the method object may move mid-codegen
	generator.descriptors = newOrdColl(32);
	generateCode(&generator);

	NativeCode *code = buildNativeCode(&generator);
	unpinCompiledCodeBytes(&generator.code);
	closeHandleScope(&scope, NULL);
	freeCodeGenerator(&generator);
	heapCodegenLockLeave(CurrentThread.heap);
	return code;
}


static NativeCode *generateBlockCode(CompiledBlock *block, CodeGenerator *parentGenerator)
{
	CodeGenerator generator;
	initCodeGenerator(&generator);
	initBlockCompiledCode(&generator.code, block);
	pinCompiledCodeBytes(&generator.code); // the method object may move mid-codegen
	generator.descriptors = newOrdColl(32);
	generateCode(&generator);

	NativeCode *code = buildNativeCode(&generator);
	unpinCompiledCodeBytes(&generator.code);
	compiledBlockSetNativeCode(block, code);
	freeCodeGenerator(&generator);
	return code;
}


static void generateCode(CodeGenerator *generator)
{
	// (x64's PROFILE_METHOD_USAGE counter is a RIP-relative trick; not ported)

	if (generator->code.header.primitive > 0) {
		generator->regsAlloc.varsSize = 1;
		generator->regsAlloc.vars[0].flags |= VAR_DEFINED | VAR_ON_STACK;
		generator->regsAlloc.vars[0].frameOffset = -2;
		generator->regsAlloc.frameSize = generator->frameSize = 2;
		generatePrimitive(generator, generator->code.header.primitive);
	}
	computeRegsAlloc(&generator->regsAlloc, &Ppc64AvailableRegs, &generator->code);
	if (!generator->regsAlloc.frameLess) {
		generatePrologue(generator, generator->regsAlloc.frameSize);
		generateContextDefinition(generator);
		// Entry safepoint poll (framed methods only), see the x64 original
		// for the full rationale.
		if (!generator->code.isBlock) {
			generateSafepointPoll(generator, 0);
		}
	} else if (generator->code.isBlock) {
		variableAt(generator, CONTEXT_INDEX)->flags |= VAR_IN_REG;
	}
	generateBody(generator);
	if (!generator->regsAlloc.frameLess) {
		generateContextRestore(generator);
		generateEpilogue(generator);
	} else {
		// Frameless methods contain no sends (RegisterAllocator guarantees
		// it) and generateCCall preserves LR, so LR still holds the caller's
		// return address here.
		asmBlr(&generator->buffer);
	}
}


static void freeCodeGenerator(CodeGenerator *generator)
{
	asmFreeBuffer(&generator->buffer);
}


// Prologue: reproduce the x64 frame EXPLICITLY (POWER has no call-pushed
// return address): parentIc (from LR) above the saved FP, slots below.
static void generatePrologue(CodeGenerator *generator, size_t frameSize)
{
	AssemblerBuffer *buffer = &generator->buffer;
	generator->frameSize = frameSize;
	asmMflr(buffer, R0);
	asmPush(buffer, R0);              // parentIc slot (x64: pushed by call)
	asmPush(buffer, FP);              // saved FP     (x64: push rbp)
	asmMr(buffer, FP, R1);            // FP = StackFrame*
	asmAddi(buffer, R1, R1, -(ptrdiff_t) (frameSize * sizeof(intptr_t)));
	// Nil-initialise the local frame slots (same GC rationale as x64).
	if (frameSize > 0) {
		generateLoadObject(buffer, Handles.nil->raw, TMP, 1);
		for (size_t i = 0; i < frameSize; i++) {
			asmStd(buffer, TMP, -(ptrdiff_t) (i + 1) * sizeof(intptr_t), FP);
		}
	}
}


static void generateEpilogue(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	asmDropStack(buffer, generator->frameSize);
	asmPop(buffer, FP);
	asmPop(buffer, R0);               // parentIc -> LR
	asmMtlr(buffer, R0);
	asmBlr(buffer);
}


static void generateContextDefinition(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	Variable *context = variableAt(generator, CONTEXT_INDEX);
	context->flags |= VAR_IN_REG;

	// spill native code entry (TGT still holds it at method entry)
	asmStd(buffer, TGT, -(ptrdiff_t) sizeof(intptr_t), FP);

	if (generator->code.isBlock) {
		// setup frame pointer inside the context
		asmStdT(buffer, FP, varOffset(RawContext, frame), varReg(context));
		spillVar(generator, context);

	} else {
		if (generator->code.header.hasContext) {
			generateMethodContextAllocation(generator, generator->code.header.contextSize);
		} else {
			// load the RUNNING worker's thread from TLS, then its dummy context
			asmLoadTls(buffer, TMP, gCurrentThreadTpoff);
			asmLd(buffer, TMP, offsetof(Thread, context), TMP);
			// spill dummy context
			asmStd(buffer, TMP, context->frameOffset * sizeof(intptr_t), FP);
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
		asmLdT(&generator->buffer, varReg(context), varOffset(RawContext, parent), varReg(context));
	}
}


// Safepoint poll, see the x64 original for the full rationale. The
// safepointRequested field is an int: load the WORD and compare (the x64
// low-byte test is a little-endian-only trick, DESIGN.md endianness rules).
static void generateSafepointPoll(CodeGenerator *generator, _Bool atBackEdge)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel noGc;
	asmInitLabel(&noGc);

	// TMP = CTX->thread->heap
	asmLdT(buffer, TMP, varOffset(RawContext, thread), CTX);
	asmLd(buffer, TMP, offsetof(Thread, heap), TMP);
	asmLwz(buffer, R0, offsetof(Heap, safepointRequested), TMP);
	asmCmpldi(buffer, 0, R0, 0);
	asmBeq(buffer, &noGc);

	// Slow path (a peer is collecting): spill the ABI's caller-saved live
	// set, park in heapGcPoll(heap, self), restore. `self` MUST be the
	// RUNNING worker (TLS), not CTX->thread (stale after a migration).
	abiEmitCallerSavedPush(gPpc64Abi, buffer);
	asmLoadTls(buffer, R4, gCurrentThreadTpoff);       // r4 = thread (C arg1)
	asmLd(buffer, R3, offsetof(Thread, heap), R4);     // r3 = heap   (C arg0)
	generator->overapproxStackmap = atBackEdge;
	generateCCall(generator, (intptr_t) heapGcPoll, 2, 1);
	generator->overapproxStackmap = 0;
	abiEmitCallerSavedPop(gPpc64Abi, buffer);

	asmPpcLabelBind(buffer, &noGc, asmOffset(buffer));
}


static void generateBody(CodeGenerator *generator)
{
	BytecodesIterator iterator;
	_Bool returns = 0;
	BytecodeLabel *labels = malloc(sizeof(BytecodeLabel) * (generator->code.bytecodesSize + 1));
	BytecodeLabel *currentLabel = labels;

	// Backward-jump (loop back-edge) bookkeeping, identical to x64.
	size_t bcSize = generator->code.bytecodesSize;
	ptrdiff_t *machineOffsetAt = malloc(sizeof(ptrdiff_t) * (bcSize + 1));
	_Bool *isBackwardTarget = calloc(bcSize + 1, sizeof(_Bool));
	{
		BytecodesIterator pre;
		bytecodeInitIterator(&pre, generator->code.bytecodes, bcSize);
		while (bytecodeHasNext(&pre)) {
			Bytecode bc = bytecodeNext(&pre);
			switch (bc) {
			case BYTECODE_COPY:
				bytecodeNextOperand(&pre);
				bytecodeNextOperand(&pre);
				break;
			case BYTECODE_SEND:
			case BYTECODE_SEND_WITH_STORE: {
				bytecodeNextByte(&pre);
				uint8_t a = bytecodeNextByte(&pre);
				bytecodeNextOperand(&pre);
				for (uint8_t k = 0; k < a; k++) {
					bytecodeNextOperand(&pre);
				}
				if (bc == BYTECODE_SEND_WITH_STORE) {
					bytecodeNextOperand(&pre);
				}
				break;
			}
			case BYTECODE_RETURN:
			case BYTECODE_OUTER_RETURN:
				bytecodeNextOperand(&pre);
				break;
			case BYTECODE_JUMP: {
				int32_t d = bytecodeNextInt32(&pre);
				ptrdiff_t t = bytecodeOffset(&pre) + d;
				if (d < 0 && t >= 0 && t <= (ptrdiff_t) bcSize) {
					isBackwardTarget[t] = 1;
				}
				break;
			}
			case BYTECODE_JUMP_NOT_MEMBER_OF: {
				bytecodeNextByte(&pre);
				bytecodeNextOperand(&pre);
				int32_t d = bytecodeNextInt32(&pre);
				ptrdiff_t t = bytecodeOffset(&pre) + d;
				if (d < 0 && t >= 0 && t <= (ptrdiff_t) bcSize) {
					isBackwardTarget[t] = 1;
				}
				break;
			}
			default:
				FAIL();
			}
		}
	}

	bytecodeInitIterator(&iterator, generator->code.bytecodes, generator->code.bytecodesSize);
	while (bytecodeHasNext(&iterator)) {
		HandleScope scope;
		openHandleScope(&scope);

		ptrdiff_t offset = bytecodeOffset(&iterator);
		machineOffsetAt[offset] = asmOffset(&generator->buffer);
		_Bool isJumpTarget = isBackwardTarget[offset];
		BytecodeLabel *label = labels;
		while (label < currentLabel) {
			if (label->offset == offset) {
				asmPpcLabelBind(&generator->buffer, &label->label, asmOffset(&generator->buffer));
				isJumpTarget = 1;
			}
			label++;
		}
		// Control-flow merge: drop reloadable register caches (see x64).
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
				movToOperand(generator, R3, bytecodeNextOperand(&iterator), 1);
			}
			break;

		case BYTECODE_RETURN:
			movOperand(generator, bytecodeNextOperand(&iterator), R3);
			if (bytecodeHasNext(&iterator)) {
				// Non-tail return: jump to the shared epilogue (r3 must
				// survive the remaining code).
				currentLabel->offset = -1;
				asmInitLabel(&currentLabel->label);
				asmB(&generator->buffer, &currentLabel->label);
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
			int32_t disp = bytecodeNextInt32(&iterator);
			ptrdiff_t target = bytecodeOffset(&iterator) + disp;
			asmInitLabel(&currentLabel->label);
			if (disp < 0) {
				// Backward (loop back-edge): bind to the known machine
				// offset; the safepoint poll's bytes are absorbed into the
				// displacement computed at emit time.
				asmPpcLabelBind(&generator->buffer, &currentLabel->label, machineOffsetAt[target]);
				currentLabel->offset = -2;
				ASSERT(!generator->regsAlloc.frameLess);
				generateSafepointPoll(generator, 1);
			} else {
				currentLabel->offset = target;
			}
			asmB(&generator->buffer, &currentLabel->label);
			currentLabel++;
			break;
		}

		case BYTECODE_JUMP_NOT_MEMBER_OF: {
			RawObject *class = compiledCodeLiteralAt(&generator->code, bytecodeNextByte(&iterator));
			Operand receiver = bytecodeNextOperand(&iterator);
			int32_t disp = bytecodeNextInt32(&iterator);
			ptrdiff_t target = bytecodeOffset(&iterator) + disp;

			asmInitLabel(&currentLabel->label);
			if (disp < 0) {
				asmPpcLabelBind(&generator->buffer, &currentLabel->label, machineOffsetAt[target]);
				currentLabel->offset = -2;
			} else {
				currentLabel->offset = target;
			}
			generateClassCheck(generator, receiver, (RawClass *) class, &currentLabel->label);
			currentLabel++;
			break;
		}

		default:
			FAIL();
		}
		closeHandleScope(&scope, NULL);
	}

	// Jumps targeting one-past-the-end (see x64).
	for (BytecodeLabel *label = labels; label < currentLabel; label++) {
		if (label->offset == (ptrdiff_t) generator->code.bytecodesSize) {
			asmPpcLabelBind(&generator->buffer, &label->label, asmOffset(&generator->buffer));
		}
	}

	if (!returns) {
		ASSERT(!generator->code.isBlock);
		movVar(generator, variableAt(generator, SELF_INDEX), R3);
	}

	// Non-tail returns land here, past the self-load.
	for (BytecodeLabel *label = labels; label < currentLabel; label++) {
		if (label->offset == -1) {
			asmPpcLabelBind(&generator->buffer, &label->label, asmOffset(&generator->buffer));
		}
	}

	free(machineOffsetAt);
	free(isBackwardTarget);
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
			generateLoadObject(&generator->buffer, Handles.nil->raw, varReg(srcVar), 1);
			srcVar->flags |= VAR_IN_REG;
		}
		movToOperand(generator, varReg(srcVar), dst, 1);
	} else if (dst.type == OPERAND_TEMP_VAR) {
		Variable *dstVar = variableAt(generator, dst.index);
		if (dstVar->reg == SPILLED_REG) {
			movOperand(generator, src, R3);
			asmStd(&generator->buffer, R3, dstVar->frameOffset * sizeof(intptr_t), FP);
			dstVar->flags |= VAR_ON_STACK;
		} else {
			movOperand(generator, src, dstVar->reg);
			dstVar->flags |= VAR_IN_REG;
			spillVar(generator, dstVar);
		}
	} else {
		movOperand(generator, src, R3);
		movToOperand(generator, R3, dst, operandMayBePointer(src));
	}
}


static _Bool rawSelectorIs(RawObject *selector, const char *name, size_t len)
{
	return rawObjectSize(selector) == len
		&& memcmp(getRawObjectIndexedVars(selector), name, len) == 0;
}

// SmallInteger selectors inlined at the call site (fast path in generateSend).
enum { ARITH_NONE = 0, ARITH_ADD, ARITH_SUB, ARITH_MUL, ARITH_DIV,
       ARITH_LT, ARITH_LE, ARITH_GT, ARITH_GE, ARITH_EQ, ARITH_NE,
       ARITH_BITAND, ARITH_BITOR, ARITH_BITXOR };

static int classifyArith(RawObject *selector)
{
	size_t n = rawObjectSize(selector);
	char *s = (char *) getRawObjectIndexedVars(selector);
	if (n == 1) {
		switch (s[0]) {
		case '+': return ARITH_ADD;
		case '-': return ARITH_SUB;
		case '*': return ARITH_MUL;
		case '/': return ARITH_DIV;   // Float only; SmallInteger keeps dispatching
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
	} else {
		if (rawSelectorIs(selector, "bitAnd:", 7)) return ARITH_BITAND;
		if (rawSelectorIs(selector, "bitOr:", 6)) return ARITH_BITOR;
		if (rawSelectorIs(selector, "bitXor:", 7)) return ARITH_BITXOR;
	}
	return ARITH_NONE;
}

static _Bool arithIsCompare(int kind) { return kind >= ARITH_LT && kind <= ARITH_NE; }
static _Bool arithIsBitOp(int kind) { return kind >= ARITH_BITAND; }


enum { IDENT_NONE = 0, IDENT_EQ, IDENT_NE, IDENT_ISNIL, IDENT_NOTNIL };

static int classifyIdentity(RawObject *selector, uint8_t argsSize)
{
	if (argsSize == 1) {
		if (rawSelectorIs(selector, "==", 2)) return IDENT_EQ;
		if (rawSelectorIs(selector, "~~", 2)) return IDENT_NE;
	} else if (argsSize == 0) {
		if (rawSelectorIs(selector, "isNil", 5)) return IDENT_ISNIL;
		if (rawSelectorIs(selector, "notNil", 6)) return IDENT_NOTNIL;
	}
	return IDENT_NONE;
}


static _Bool floatInlineEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_NO_INLINE_FLOAT") == NULL;
	}
	return enabled;
}


// Map an ordered-compare arith kind to the branch-if-taken (BO, BI) pair for
// a cr0 cmpd/fcmpu result. NaN handling for floats is a SEPARATE dedicated
// branch on cr0.SO (fcmpu's unordered bit), cleaner than x64's parity flag.
static void arithCompareBranch(int arithKind, int *bo, int *bi)
{
	switch (arithKind) {
	case ARITH_LT: *bo = BO_IF_TRUE;  *bi = CR_LT; break;
	case ARITH_LE: *bo = BO_IF_FALSE; *bi = CR_GT; break;
	case ARITH_GT: *bo = BO_IF_TRUE;  *bi = CR_GT; break;
	case ARITH_GE: *bo = BO_IF_FALSE; *bi = CR_LT; break;
	case ARITH_EQ: *bo = BO_IF_TRUE;  *bi = CR_EQ; break;
	default:       *bo = BO_IF_FALSE; *bi = CR_EQ; break;  // ARITH_NE
	}
}



// Raw bit move GPR -> FPR, branched on the CAPABILITY (hasGprVsrMoves =
// the ARCH_2_07 hwcap2 feature bit, cumulative across P8/P9/P10 and future
// levels): mtvsrd when present, else the per-thread scratch slot reached via
// TLS in `tls` (straight-line code, no yield point, so fiber migration cannot
// split the store from the load). 970/POWER7 take the memory path.
static void generateBitsToFpr(AssemblerBuffer *buffer, Register src, int fpr, Register tls)
{
	if (gPpc64Cpu.hasGprVsrMoves) {
		asmMtvsrd(buffer, fpr, src);
		return;
	}
	asmStd(buffer, src, offsetof(Thread, jitFpuScratch), tls);
	asmLfd(buffer, fpr, offsetof(Thread, jitFpuScratch), tls);
}


// Raw bit move FPR -> GPR (mfvsrd on ISA 2.07, TLS scratch on the baseline).
static void generateFprToBits(AssemblerBuffer *buffer, int fpr, Register dst, Register tls)
{
	if (gPpc64Cpu.hasGprVsrMoves) {
		asmMfvsrd(buffer, dst, fpr);
		return;
	}
	asmStfd(buffer, fpr, offsetof(Thread, jitFpuScratch), tls);
	asmLd(buffer, dst, offsetof(Thread, jitFpuScratch), tls);
}


// Load one already-guarded Float operand (SmallFloat64 immediate OR
// BoxedFloat64 pointer) into an FPR; cannot miss after the guard phase.
// offsetReg holds SMALLFLOAT_OFFSET and is preserved; R0 is the scratch;
// reg itself stays intact.
static void generateFloatOperandLoad(AssemblerBuffer *buffer, Register reg, int fpr,
	Register offsetReg, Register tls)
{
	AssemblerLabel boxed, skipAdd, done;
	asmInitLabel(&boxed);
	asmInitLabel(&skipAdd);
	asmInitLabel(&done);

	asmAndiDot(buffer, R0, reg, 2);          // bit 1 set here means immediate (0b11)
	asmBeq(buffer, &boxed);

	// SmallFloat64 decode, the inverse of tagFloat (see Object.h): payload =
	// value >> 2; bits = ROR64(payload <= 1 ? payload : payload + offset, 1)
	asmSrdi(buffer, R0, reg, 2);
	asmCmpldi(buffer, 0, R0, 1);
	asmBle(buffer, &skipAdd);                // payloads 0/1 are +-0.0: no rebias
	asmAdd(buffer, R0, R0, offsetReg);
	asmPpcLabelBind(buffer, &skipAdd, asmOffset(buffer));
	asmRotrdi(buffer, R0, R0, 1);
	generateBitsToFpr(buffer, R0, fpr, tls);
	asmB(buffer, &done);

	asmPpcLabelBind(buffer, &boxed, asmOffset(buffer));
	asmLfd(buffer, fpr, varOffset(RawFloat, value), reg);

	asmPpcLabelBind(buffer, &done, asmOffset(buffer));
}


// Inline Float fast path, see the x64 original. Operands on the stack
// (receiver at 0(r1), arg at 8(r1)), TMP still holds the receiver on every
// fall-through-to-dispatch path. Each operand may be a SmallFloat64 immediate
// (decoded inline) or a BoxedFloat64 (lfd); an arithmetic result that fits the
// immediate range is encoded inline with NO allocation, and only out-of-range
// results take the allocate-first boxed path (which re-decodes the reloaded
// operands: no FPR value survives the stub). GPR<->FPR moves use mtvsrd/mfvsrd
// on ISA 2.07+, else the per-thread TLS scratch (970/POWER7 baseline). The
// result lands in r3 on every committed path.
// litRcvBits / litArgBits, when non-NULL, are the IEEE bits of a SmallFloat64
// LITERAL receiver / argument (OPERAND_VALUE tagged 0b11), known at codegen
// time: that operand needs no guard (it cannot miss) and no runtime decode,
// the constant is materialized straight into its FPR (li64 + the GPR->FPR
// move), both here and in the re-decode after the boxing stub. The bits are
// plain data, not a heap pointer: nothing to re-read, nothing for the GC to
// see. With BOTH operands literal no guard is emitted at all (not even the
// class load) and the fast path can never fall to dispatch; the unreferenced
// miss labels bind as no-ops.
static void generateFloatFastPath(CodeGenerator *generator, int arithKind,
	AssemblerLabel *arithMerge, const uint64_t *litRcvBits, const uint64_t *litArgBits)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel tagMissR, tagMissA, immOkR, immOkA, classMissR, classMissA;
	asmInitLabel(&tagMissR);
	asmInitLabel(&tagMissA);
	asmInitLabel(&immOkR);
	asmInitLabel(&immOkA);
	asmInitLabel(&classMissR);
	asmInitLabel(&classMissA);

	if (litRcvBits == NULL) {
		asmLd(buffer, R3, 0, R1);                // receiver
	}
	if (litArgBits == NULL) {
		asmLd(buffer, R4, sizeof(intptr_t), R1); // arg
	}
	// Guard phase, the ONLY phase that can fall to dispatch: each operand must
	// be a SmallFloat64 immediate (both tag bits set) or a pointer whose class
	// word is BoxedFloat64. A literal operand is a known immediate: no guard.
	if (litRcvBits == NULL || litArgBits == NULL) {
		generateLoadObject(buffer, (RawObject *) Handles.BoxedFloat64->raw, R6, 0); // BoxedFloat64 class (raw)
	}
	if (litRcvBits == NULL) {
		asmAndiDot(buffer, R0, R3, 1);
		asmBeq(buffer, &tagMissR);           // 0b00/0b10: not a Float
		asmAndiDot(buffer, R0, R3, 2);
		asmBne(buffer, &immOkR);             // 0b11: immediate
		asmLdT(buffer, R0, varOffset(RawObject, class), R3);
		asmCmpd(buffer, 0, R0, R6);
		asmBne(buffer, &classMissR);
		asmPpcLabelBind(buffer, &immOkR, asmOffset(buffer));
	}
	if (litArgBits == NULL) {
		asmAndiDot(buffer, R0, R4, 1);
		asmBeq(buffer, &tagMissA);
		asmAndiDot(buffer, R0, R4, 2);
		asmBne(buffer, &immOkA);
		asmLdT(buffer, R0, varOffset(RawObject, class), R4);
		asmCmpd(buffer, 0, R0, R6);
		asmBne(buffer, &classMissA);
		asmPpcLabelBind(buffer, &immOkA, asmOffset(buffer));
	}

	// Committed: decode both operands into f0/f1. R5 = SMALLFLOAT_OFFSET
	// (0x6 << 60); TMP2 = TLS base for the baseline GPR<->FPR path.
	asmLi(buffer, R5, 6);
	asmSldi(buffer, R5, R5, 60);
	if (!gPpc64Cpu.hasGprVsrMoves) {
		asmLoadTls(buffer, TMP2, gCurrentThreadTpoff);
	}
	if (litRcvBits == NULL) {
		generateFloatOperandLoad(buffer, R3, 0, R5, TMP2);
	} else {
		asmLi64(buffer, R0, *litRcvBits);
		generateBitsToFpr(buffer, R0, 0, TMP2);
	}
	if (litArgBits == NULL) {
		generateFloatOperandLoad(buffer, R4, 1, R5, TMP2);
	} else {
		asmLi64(buffer, R0, *litArgBits);
		generateBitsToFpr(buffer, R0, 1, TMP2);
	}

	if (arithIsCompare(arithKind)) {
		// No allocation: unbox both, fcmpu, load true/false. Unordered (NaN)
		// is a DEDICATED cr0 bit (SO) on POWER: branch it to firstBlock
		// (false for < <= > >= =, true for ~=), then branch the main
		// condition to secondBlock.
		asmFcmpu(buffer, 0, 0, 1);
		AssemblerLabel firstBlock, secondBlock, cmpDone;
		asmInitLabel(&firstBlock);
		asmInitLabel(&secondBlock);
		asmInitLabel(&cmpDone);
		int bo, bi;
		RawObject *firstValue, *secondValue;
		if (arithKind == ARITH_NE) {
			bo = BO_IF_TRUE; bi = CR_EQ;           // equal -> secondBlock(false)
			firstValue = Handles.true->raw;
			secondValue = Handles.false->raw;
		} else {
			arithCompareBranch(arithKind, &bo, &bi);
			firstValue = Handles.false->raw;
			secondValue = Handles.true->raw;
		}
		asmBc(buffer, BO_IF_TRUE, CR_SO, &firstBlock);  // unordered (NaN)
		asmBc(buffer, bo, bi, &secondBlock);
		asmPpcLabelBind(buffer, &firstBlock, asmOffset(buffer));
		generateLoadObject(buffer, firstValue, R3, 1);
		asmB(buffer, &cmpDone);
		asmPpcLabelBind(buffer, &secondBlock, asmOffset(buffer));
		generateLoadObject(buffer, secondValue, R3, 1);
		asmPpcLabelBind(buffer, &cmpDone, asmOffset(buffer));
	} else {
		// Arithmetic: compute FIRST, then try the immediate encode; results
		// that fit the SmallFloat64 range allocate NOTHING.
		switch (arithKind) {
		case ARITH_ADD: asmFadd(buffer, 0, 0, 1); break;
		case ARITH_SUB: asmFsub(buffer, 0, 0, 1); break;
		case ARITH_MUL: asmFmul(buffer, 0, 0, 1); break;
		default:        asmFdiv(buffer, 0, 0, 1); break;   // ARITH_DIV
		}

		AssemblerLabel payloadReady, boxA, boxB, fastDone;
		asmInitLabel(&payloadReady);
		asmInitLabel(&boxA);
		asmInitLabel(&boxB);
		asmInitLabel(&fastDone);

		// SmallFloat64 encode (tagFloat, see Object.h): rot = ROL64(bits, 1);
		// rot <= 1 is +-0.0 and IS the payload; otherwise payload =
		// rot - offset and must land in [2, 2^62).
		generateFprToBits(buffer, 0, R0, TMP2);
		asmRotldi(buffer, R0, R0, 1);
		asmCmpldi(buffer, 0, R0, 1);
		asmBle(buffer, &payloadReady);
		asmSubf(buffer, R0, R5, R0);             // payload = rot - offset
		asmCmpldi(buffer, 0, R0, 1);
		asmBle(buffer, &boxA);                   // +-2^-255: collides with +-0.0, box
		asmSrdi(buffer, R6, R0, 62);
		asmCmpldi(buffer, 0, R6, 0);
		asmBne(buffer, &boxB);                   // out of range or underflow wrap: box
		asmPpcLabelBind(buffer, &payloadReady, asmOffset(buffer));
		asmSldi(buffer, R0, R0, 2);
		asmOri(buffer, R3, R0, 3);               // tagged immediate result in r3
		asmB(buffer, &fastDone);

		// Out-of-range result: allocate FIRST (the operands survive the
		// possibly-scavenging stub via its stackmap; FPR values do NOT), then
		// re-decode the reloaded operands and redo the op into the new box.
		asmPpcLabelBind(buffer, &boxA, asmOffset(buffer));
		asmPpcLabelBind(buffer, &boxB, asmOffset(buffer));
		generateLoadObject(buffer, (RawObject *) Handles.BoxedFloat64->raw, R4, 0);
		asmLi(buffer, R5, 0);
		generateStubCall(generator, &AllocateStub);   // r3 = new tagged Float
		if (litRcvBits == NULL) {
			asmLd(buffer, R4, 0, R1);                 // reload receiver
		}
		if (litArgBits == NULL) {
			asmLd(buffer, R6, sizeof(intptr_t), R1);  // reload arg
		}
		if (litRcvBits == NULL || litArgBits == NULL) {
			asmLi(buffer, R5, 6);                     // offset again (R5 was the stub arg)
			asmSldi(buffer, R5, R5, 60);
		}
		if (!gPpc64Cpu.hasGprVsrMoves) {
			asmLoadTls(buffer, TMP2, gCurrentThreadTpoff);
		}
		if (litRcvBits == NULL) {
			generateFloatOperandLoad(buffer, R4, 0, R5, TMP2);
		} else {
			asmLi64(buffer, R0, *litRcvBits);
			generateBitsToFpr(buffer, R0, 0, TMP2);
		}
		if (litArgBits == NULL) {
			generateFloatOperandLoad(buffer, R6, 1, R5, TMP2);
		} else {
			asmLi64(buffer, R0, *litArgBits);
			generateBitsToFpr(buffer, R0, 1, TMP2);
		}
		switch (arithKind) {
		case ARITH_ADD: asmFadd(buffer, 0, 0, 1); break;
		case ARITH_SUB: asmFsub(buffer, 0, 0, 1); break;
		case ARITH_MUL: asmFmul(buffer, 0, 0, 1); break;
		default:        asmFdiv(buffer, 0, 0, 1); break;   // ARITH_DIV
		}
		asmStfd(buffer, 0, varOffset(RawFloat, value), R3);
		asmPpcLabelBind(buffer, &fastDone, asmOffset(buffer));
	}
	asmB(buffer, arithMerge);

	// Any type mismatch falls through to the dispatch that follows.
	asmPpcLabelBind(buffer, &tagMissR, asmOffset(buffer));
	asmPpcLabelBind(buffer, &tagMissA, asmOffset(buffer));
	asmPpcLabelBind(buffer, &classMissR, asmOffset(buffer));
	asmPpcLabelBind(buffer, &classMissA, asmOffset(buffer));
}


// r3: receiver class (tagged, from generateLoadClass)
// TGT: native code target out
// Per-site inline cache for the dynamic send (see the x64 twin and
// jit/InlineCache.h). The cell is DATA reached by ordinary ld: the li64
// non-atomicity never matters (baked once, pre-publication) and no icache
// flush is involved. state->class and state->target are address-dependent
// loads off the state pointer, which POWER orders for free against the
// CAS-release publishes; no isync needed. Any way-0 miss goes through the
// shared PicProbeStub (pic walk, mega global probe, or C transition).
static void generateIcSend(CodeGenerator *generator, uint8_t selectorIndex)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel miss, callFromHit;
	asmInitLabel(&miss);
	asmInitLabel(&callFromHit);

	// Cell address: li64 placeholder (fixed 5-instruction shape), baked exactly
	// once in buildNativeCodeFromAssembler, never patched after publication
	// (NOT a pointersOffsets entry: the GC must not forward it).
	asmLi64(buffer, TMP2, 0);
	asmAddIcSite(buffer, asmOffset(buffer) - 20);
	asmLd(buffer, R7, offsetof(IcCell, state), TMP2);      // state
	asmLd(buffer, R0, offsetof(IcState, class), R7);       // state->class
	asmCmpd(buffer, 0, R0, R3);
	asmBne(buffer, &miss);
	if (icStatsEnabled()) {
		asmLi64(buffer, R6, (uint64_t) (uintptr_t) &gIcStats.hits);
		asmLd(buffer, R5, 0, R6);
		asmAddi(buffer, R5, R5, 1);
		asmStd(buffer, R5, 0, R6);
	}
	asmLd(buffer, TGT, offsetof(IcState, target), R7);
	asmB(buffer, &callFromHit);

	asmPpcLabelBind(buffer, &miss, asmOffset(buffer));
	generateLoadObject(buffer,
		compiledCodeLiteralAt(&generator->code, selectorIndex), R4, 0);
	asmMr(buffer, R5, TMP2);                    // cell = 3rd C arg (class stays tagged)
	generateStubCall(generator, &PicProbeStub); // clobbers TMP; entry back in TGT

	asmPpcLabelBind(buffer, &callFromHit, asmOffset(buffer));
}


// TMP: receiver | r3: receiver class (lookup arg) | r4: selector | r3: result
static void generateSend(CodeGenerator *generator, BytecodesIterator *iterator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	// Keep the literal INDEX, not the raw selector pointer: pushOperand of a
	// BLOCK argument compiles the whole block right here, which allocates and
	// can scavenge; a raw pointer captured before that would be baked stale
	// below (see x64 generateSend). Classification runs BEFORE the pushes.
	uint8_t selectorIndex = bytecodeNextByte(iterator);
	uint8_t argsSize = bytecodeNextByte(iterator);
	Operand receiver = bytecodeNextOperand(iterator);
	int arithKind = argsSize == 1
		? classifyArith(compiledCodeLiteralAt(&generator->code, selectorIndex)) : ARITH_NONE;
	int identKind = classifyIdentity(compiledCodeLiteralAt(&generator->code, selectorIndex), argsSize);

	// A SmallFloat64 LITERAL receiver or argument (OPERAND_VALUE tagged 0b11)
	// is known at codegen time: the float fast path materializes it as a
	// constant and skips its guard and decode, and the SmallInteger fast path
	// is dead (that operand's tag test can never pass). The Value is an
	// immediate, not a heap pointer, so holding its bits across the pushes is
	// GC-safe (see the x64 original).
	_Bool floatLitRcv = receiver.type == OPERAND_VALUE
		&& valueTypeOf(receiver.value, VALUE_FLOAT);
	uint64_t litRcvBits = floatLitRcv ? doubleToBits(floatValueOf(receiver.value)) : 0;
	_Bool floatLitArg = 0;
	uint64_t litArgBits = 0;
	for (uint8_t i = 0; i < argsSize; i++) {
		Operand arg = bytecodeNextOperand(iterator);
		if (i == 0 && argsSize == 1 && arg.type == OPERAND_VALUE
				&& valueTypeOf(arg.value, VALUE_FLOAT)) {
			floatLitArg = 1;
			litArgBits = doubleToBits(floatValueOf(arg.value));
		}
		pushOperand(generator, arg);
	}
	movOperand(generator, receiver, TMP);
	asmPush(buffer, TMP);
	generator->frameSize++;

	// --- inline arithmetic/comparison fast paths (see x64) ------------------
	_Bool identityInline = identKind != IDENT_NONE;
	// int has no inline '/'; a float-literal operand fails its tag test always
	_Bool intInline = arithKind != ARITH_NONE && arithKind != ARITH_DIV
		&& !floatLitArg && !floatLitRcv;
	_Bool floatInline = arithKind != ARITH_NONE && !arithIsBitOp(arithKind) && floatInlineEnabled();
	_Bool anyInline = intInline || floatInline || identityInline;
	AssemblerLabel arithMerge, floatMerge;
	if (intInline) {
		asmInitLabel(&arithMerge);
	}
	if (floatInline) {
		asmInitLabel(&floatMerge);
	}

	if (intInline) {
		AssemblerLabel tagMissR, tagMissA, overflowMiss;
		asmInitLabel(&tagMissR);
		asmInitLabel(&tagMissA);
		asmInitLabel(&overflowMiss);
		asmLd(buffer, R3, 0, R1);                     // receiver
		asmLd(buffer, R4, sizeof(intptr_t), R1);      // arg
		asmAndiDot(buffer, R0, R3, 3);                // receiver SmallInt?
		asmBne(buffer, &tagMissR);
		asmAndiDot(buffer, R0, R4, 3);                // arg SmallInt?
		asmBne(buffer, &tagMissA);
		switch (arithKind) {
		case ARITH_ADD:
			// XER[SO] is sticky: a stale bit only causes a false-positive
			// fall-through to dispatch (slower, still correct); the miss
			// join below re-arms it. DESIGN.md, overflow protocol.
			asmAddoDot(buffer, R3, R3, R4);
			asmBso(buffer, &overflowMiss);            // -> LargeInteger via send
			break;
		case ARITH_SUB:
			asmSubfoDot(buffer, R3, R4, R3);          // r3 = r3 - r4
			asmBso(buffer, &overflowMiss);
			break;
		case ARITH_MUL:
			asmSradi(buffer, R3, R3, 2);              // untag receiver -> a
			asmMulldoDot(buffer, R3, R3, R4);         // a * (b<<2) = (a*b)<<2
			asmBso(buffer, &overflowMiss);
			break;
		case ARITH_BITAND:
			asmAnd(buffer, R3, R3, R4);               // stays tagged
			break;
		case ARITH_BITOR:
			asmOr(buffer, R3, R3, R4);
			break;
		case ARITH_BITXOR:
			asmXor(buffer, R3, R3, R4);
			break;
		default: {   // comparisons
			AssemblerLabel cmpTrue, cmpDone;
			int bo, bi;
			arithCompareBranch(arithKind, &bo, &bi);
			asmInitLabel(&cmpTrue);
			asmInitLabel(&cmpDone);
			asmCmpd(buffer, 0, R3, R4);               // receiver ? arg
			asmBc(buffer, bo, bi, &cmpTrue);
			generateLoadObject(buffer, Handles.false->raw, R3, 1);
			asmB(buffer, &cmpDone);
			asmPpcLabelBind(buffer, &cmpTrue, asmOffset(buffer));
			generateLoadObject(buffer, Handles.true->raw, R3, 1);
			asmPpcLabelBind(buffer, &cmpDone, asmOffset(buffer));
			break;
		}
		}
		asmB(buffer, &arithMerge);
		// tag/overflow misses fall through to the Float path (or dispatch);
		// re-arm the sticky XER[SO] on the way.
		asmPpcLabelBind(buffer, &tagMissR, asmOffset(buffer));
		asmPpcLabelBind(buffer, &tagMissA, asmOffset(buffer));
		asmPpcLabelBind(buffer, &overflowMiss, asmOffset(buffer));
		asmClearXerSo(buffer);
	}

	if (floatInline) {
		generateFloatFastPath(generator, arithKind, &floatMerge,
			floatLitRcv ? &litRcvBits : NULL, floatLitArg ? &litArgBits : NULL);
	}

	if (identityInline) {
		AssemblerLabel identTrue, identDone;
		asmInitLabel(&identTrue);
		asmInitLabel(&identDone);
		asmLd(buffer, R3, 0, R1);                          // receiver
		if (argsSize == 1) {
			asmLd(buffer, R4, sizeof(intptr_t), R1);       // arg
		} else {
			generateLoadObject(buffer, Handles.nil->raw, R4, 1);
		}
		asmCmpd(buffer, 0, R3, R4);
		_Bool wantEqual = identKind == IDENT_EQ || identKind == IDENT_ISNIL;
		asmBc(buffer, wantEqual ? BO_IF_TRUE : BO_IF_FALSE, CR_EQ, &identTrue);
		generateLoadObject(buffer, Handles.false->raw, R3, 1);
		asmB(buffer, &identDone);
		asmPpcLabelBind(buffer, &identTrue, asmOffset(buffer));
		generateLoadObject(buffer, Handles.true->raw, R3, 1);
		asmPpcLabelBind(buffer, &identDone, asmOffset(buffer));
		generator->frameSize -= argsSize + 1;
	} else {
		RawClass *class = compiledCodeResolveOperandClass(&generator->code, receiver);
		if (class != NULL) {
			asmLi64(buffer, TGT, (uint64_t) (uintptr_t) lookupNativeCode(class,
				(RawString *) compiledCodeLiteralAt(&generator->code, selectorIndex)));
		} else {
			// Always recompute the receiver's class from TMP (see x64).
			generateLoadClass(buffer, TMP, R3);
			if (icEnabled()) {
				generateIcSend(generator, selectorIndex);
			} else {
				// ST_NO_IC: exactly the pre-IC sequence
				generateLoadObject(buffer,
					compiledCodeLiteralAt(&generator->code, selectorIndex), R4, 0);
				generateMethodLookup(generator);
			}
		}

		generator->frameSize -= argsSize + 1;
		asmCallReg(buffer, TGT);
		generateStackmap(generator);
		ordCollAdd(generator->descriptors, createBytecodeDescriptor(asmOffset(&generator->buffer), generator->bytecodeNumber));
	}
	// Fast paths and the dispatched call converge here (result in r3).
	if (anyInline) {
		ptrdiff_t mergeOffset = asmOffset(buffer);
		if (intInline) {
			asmPpcLabelBind(buffer, &arithMerge, mergeOffset);
		}
		if (floatInline) {
			asmPpcLabelBind(buffer, &floatMerge, mergeOffset);
		}
	}
	asmDropStack(buffer, argsSize + 1);
	invalidateRegs(&generator->regsAlloc);
}


void generateStoreCheck(CodeGenerator *generator, Register object, Register value)
{
	ASSERT(object != TMP && value != TMP);
	ASSERT(object != TMP2 && value != TMP2);
	ASSERT(object != R0 && value != R0);
	AssemblerBuffer *buffer = &generator->buffer;
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

	ptrdiff_t rememberedSetOffset = offsetof(Thread, rememberedSet);
	ptrdiff_t blocksOffset = rememberedSetOffset + offsetof(RememberedSet, blocks);
	ptrdiff_t tagsOffset = varOffset(RawObject, tags);

	// Fast-path skips (see x64): only an old object gaining a pointer to a
	// young, not-yet-remembered object needs recording.
	asmAndiDot(buffer, R0, object, NEW_SPACE_TAG);
	asmBne(buffer, &objectIsNew);
	// value must be a heap pointer (tag 0b01 exactly): bit 0 alone would also
	// remember a VALUE_FLOAT immediate (0b11)
	asmAndiDot(buffer, R0, value, 3);
	asmCmpldi(buffer, 0, R0, VALUE_POINTER);
	asmBne(buffer, &valueIsNotPtr);
	asmAndiDot(buffer, R0, value, NEW_SPACE_TAG);
	asmBeq(buffer, &valueIsOld);
	asmLbz(buffer, R0, tagsOffset, object);
	asmAndiDot(buffer, R0, R0, TAG_REMEMBERED);
	asmBne(buffer, &alreadyInSet);

	// mark as remembered (byte RMW via r0, logical ops read r0 as a
	// register, unlike D-form bases)
	asmLbz(buffer, R0, tagsOffset, object);
	asmOri(buffer, R0, R0, TAG_REMEMBERED);
	asmStb(buffer, R0, tagsOffset, object);

	// TMP = the RUNNING worker's remembered-set head block (TLS)
	asmLoadTls(buffer, TMP, gCurrentThreadTpoff);
	asmLd(buffer, TMP, blocksOffset, TMP);

	// Grow BEFORE the store when the block is full (see x64). r0 = end,
	// TMP2 = current, no push/pop dance needed (both are free scratch).
	asmLd(buffer, R0, offsetof(RememberedSetBlock, end), TMP);
	asmLd(buffer, TMP2, offsetof(RememberedSetBlock, current), TMP);
	asmCmpld(buffer, 0, R0, TMP2);
	asmBgt(buffer, &notFull);              // end > current -> room

	// grow via the C helper, then reload the head block
	abiEmitCallerSavedPush(gPpc64Abi, buffer);
	asmLoadTls(buffer, TMP, gCurrentThreadTpoff);
	asmAddi(buffer, R3, TMP, rememberedSetOffset);
	generateCCall(generator, (intptr_t) rememberedSetGrow, 1, 0);
	abiEmitCallerSavedPop(gPpc64Abi, buffer);
	asmLoadTls(buffer, TMP, gCurrentThreadTpoff);
	asmLd(buffer, TMP, blocksOffset, TMP);

	asmPpcLabelBind(buffer, &notFull, asmOffset(buffer));
	// store: advance block->current, then write object at the old slot
	asmLd(buffer, TMP2, offsetof(RememberedSetBlock, current), TMP);
	asmAddi(buffer, TMP2, TMP2, sizeof(intptr_t));
	asmStd(buffer, TMP2, offsetof(RememberedSetBlock, current), TMP);
	asmStd(buffer, object, -(ptrdiff_t) sizeof(intptr_t), TMP2);

	asmPpcLabelBind(buffer, &objectIsNew, asmOffset(buffer));
	asmPpcLabelBind(buffer, &valueIsNotPtr, asmOffset(buffer));
	asmPpcLabelBind(buffer, &valueIsOld, asmOffset(buffer));
	asmPpcLabelBind(buffer, &alreadyInSet, asmOffset(buffer));
}

// r3: class (tagged in, untagged for the C call)
// r4: selector (raw)
// r5: class-and-selector hash
// TGT: native code out
void generateMethodLookup(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	AssemblerLabel lookup;
	AssemblerLabel cache;
	AssemblerLabel call;

	asmInitLabel(&lookup);
	asmInitLabel(&cache);
	asmInitLabel(&call);

	asmAddi(buffer, R3, R3, -1);   // untag class (also the C arg for the stub)

	// hash class and selector, must match lookupHash() in Lookup.h
	asmXor(buffer, R5, R3, R4);
	asmSrdi(buffer, R5, R5, 4);
	asmAndiDot(buffer, R5, R5, LOOKUP_CACHE_SIZE - 1);

	// probe the RUNNING worker's own TLS LookupCache (see Lookup.h). The
	// classes/selectors/codes arrays are 32 KB apart, past the DS-form
	// range, so step the probe pointer with addis (+65536) instead of
	// re-scaling: r6 = &classes[i]; r6+65536-32768 = &selectors[i];
	// r6+65536 = &codes[i]. (DESIGN.md, lookup probe.)
	asmLoadTls(buffer, TMP, gLookupCacheTpoff);
	asmSldi(buffer, R0, R5, 3);
	asmAdd(buffer, R6, TMP, R0);
	asmLd(buffer, R0, 0, R6);                       // classes[i]
	asmCmpd(buffer, 0, R0, R3);
	asmBne(buffer, &lookup);
	asmAddis(buffer, R6, R6, 1);                    // r6 += 65536
	asmLd(buffer, R0, -32768, R6);                  // selectors[i]
	asmCmpd(buffer, 0, R0, R4);
	asmBeq(buffer, &cache);

	// not in cache -> lookup
	asmPpcLabelBind(buffer, &lookup, asmOffset(buffer));
	generateStubCall(generator, &LookupStub);
	asmB(buffer, &call);

	// load code from cache
	asmPpcLabelBind(buffer, &cache, asmOffset(buffer));
	asmLd(buffer, TGT, 0, R6);                      // codes[i]

	asmPpcLabelBind(buffer, &call, asmOffset(buffer));
}


void generateLoadObject(AssemblerBuffer *buffer, RawObject *object, Register dst, _Bool tag)
{
	int64_t ptr = tag ? tagPtr(object) : (int64_t) object;
	asmLi64(buffer, dst, (uint64_t) ptr);
	if (!isOldObject(object)) {
		// The GC patches the 4 halfword immediates of the whole li64
		// sequence, the offset points at its FIRST byte (TargetCodePatch).
		asmAddPointerOffset(buffer, asmOffset(buffer) - 20);
	}
}


void generateLoadClass(AssemblerBuffer *buffer, Register src, Register dst)
{
	// Four-way exact dispatch on the 2-bit tag. The old shortcut (above
	// VALUE_POINTER means Character) would classify a VALUE_FLOAT immediate
	// (0b11) as a Character. One end label per arm: a label takes a single
	// forward reference.
	AssemblerLabel pointer;
	AssemblerLabel character;
	AssemblerLabel floatImm;
	AssemblerLabel endInt, endPtr, endChar;

	asmInitLabel(&pointer);
	asmInitLabel(&character);
	asmInitLabel(&floatImm);
	asmInitLabel(&endInt);
	asmInitLabel(&endPtr);
	asmInitLabel(&endChar);

	asmAndiDot(buffer, dst, src, 3);
	asmCmpldi(buffer, 0, dst, VALUE_POINTER);
	asmBeq(buffer, &pointer);
	asmCmpldi(buffer, 0, dst, VALUE_CHAR);
	asmBeq(buffer, &character);
	asmCmpldi(buffer, 0, dst, VALUE_FLOAT);
	asmBeq(buffer, &floatImm);

	generateLoadObject(buffer, (RawObject *) Handles.SmallInteger->raw, dst, 1);
	asmB(buffer, &endInt);

	asmPpcLabelBind(buffer, &pointer, asmOffset(buffer));
	asmLdT(buffer, dst, -1, src);   // class word of the tagged pointer
	asmAddi(buffer, dst, dst, 1);   // tag the class
	asmB(buffer, &endPtr);

	asmPpcLabelBind(buffer, &character, asmOffset(buffer));
	generateLoadObject(buffer, (RawObject *) Handles.Character->raw, dst, 1);
	asmB(buffer, &endChar);

	asmPpcLabelBind(buffer, &floatImm, asmOffset(buffer));
	generateLoadObject(buffer, (RawObject *) Handles.SmallFloat64->raw, dst, 1);

	asmPpcLabelBind(buffer, &endInt, asmOffset(buffer));
	asmPpcLabelBind(buffer, &endPtr, asmOffset(buffer));
	asmPpcLabelBind(buffer, &endChar, asmOffset(buffer));
}


static void generateOuterReturn(CodeGenerator *generator, BytecodesIterator *iterator)
{
	AssemblerLabel deathContext;
	AssemblerLabel cut;
	asmInitLabel(&deathContext);
	asmInitLabel(&cut);

	Variable *context = variableAt(generator, CONTEXT_INDEX);
	AssemblerBuffer *buffer = &generator->buffer;

	movOperand(generator, bytecodeNextOperand(iterator), R3);

	fillVar(generator, context);
	// load home context
	asmLdT(buffer, varReg(context), varOffset(RawContext, home), varReg(context));
	// load home stack frame
	asmLdT(buffer, TMP, varOffset(RawContext, frame), varReg(context));
	// check frame validity: home frame's context slot must still point at it
	asmLd(buffer, R0, -2 * (ptrdiff_t) sizeof(intptr_t), TMP);
	asmCmpd(buffer, 0, R0, varReg(context));
	asmBne(buffer, &deathContext);

	// pending ensure:/ifCurtailed: cleanups? (R6 is dead: the return leaves)
	asmLoadTls(buffer, R6, gCurrentThreadTpoff);
	asmLd(buffer, R6, offsetof(Thread, unwindHandler), R6);
	asmCmpdi(buffer, 0, R6, 0);
	asmBeq(buffer, &cut);

	// slow path: run the cleanups below the home frame in C (r3 = the result
	// already; the helper keeps it alive across cleanup-triggered GCs and
	// answers it, possibly moved, in r3), then re-derive the home context from
	// this frame's context slot, which the GC keeps fresh (the raw home-frame
	// address itself is stable, but the context objects can move).
	asmMr(buffer, R4, TMP); // home frame
	generateCCall(generator, (intptr_t) unwindReturning, 2, 1);
	asmLd(buffer, varReg(context), -2 * (ptrdiff_t) sizeof(intptr_t), FP);
	asmLdT(buffer, varReg(context), varOffset(RawContext, home), varReg(context));
	asmLdT(buffer, TMP, varOffset(RawContext, frame), varReg(context));

	// context is live: unwind straight to the home frame and return
	asmPpcLabelBind(buffer, &cut, asmOffset(buffer));
	asmMr(buffer, R1, TMP);
	asmPop(buffer, FP);
	asmPop(buffer, R0);
	asmMtlr(buffer, R0);
	asmBlr(buffer);

	// dead context: invoke #cannotReturn:
	asmPpcLabelBind(buffer, &deathContext, asmOffset(buffer));
	asmPush(buffer, R3);
	asmPush(buffer, varReg(context));
	generator->frameSize += 2;
	generateLoadObject(buffer, (RawObject *) Handles.MethodContext->raw, R3, 1);
	generateLoadObject(buffer, (RawObject *) Handles.cannotReturnSymbol->raw, R4, 0);
	generateMethodLookup(generator);
	generator->frameSize -= 2;
	asmCallReg(buffer, TGT);
	generateStackmap(generator);
	asmDropStack(buffer, 2);
}


static void pushOperand(CodeGenerator *generator, Operand operand)
{
	AssemblerBuffer *buffer = &generator->buffer;

	switch (operand.type) {
	case OPERAND_VALUE:
		emitLoadImm(buffer, TMP, operand.value);
		asmPush(buffer, TMP);
		break;

	case OPERAND_NIL:
		generateLoadObject(buffer, Handles.nil->raw, TMP, 1);
		asmPush(buffer, TMP);
		break;

	case OPERAND_TRUE:
		generateLoadObject(buffer, Handles.true->raw, TMP, 1);
		asmPush(buffer, TMP);
		break;

	case OPERAND_FALSE:
		generateLoadObject(buffer, Handles.false->raw, TMP, 1);
		asmPush(buffer, TMP);
		break;

	case OPERAND_THIS_CONTEXT: {
		Variable *context = variableAt(generator, CONTEXT_INDEX);
		fillVar(generator, context);
		asmPush(buffer, varReg(context));
		break;
	}

	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:;
		Variable *var = variableAt(generator, operand.index);
		if (var->flags & VAR_IN_REG) {
			asmPush(buffer, varReg(var));
		} else if (var->flags & VAR_ON_STACK) {
			asmLd(buffer, TMP, var->frameOffset * sizeof(intptr_t), FP);
			asmPush(buffer, TMP);
		} else if (var->reg == SPILLED_REG) {
			generateLoadObject(buffer, Handles.nil->raw, TMP, 1);
			asmPush(buffer, TMP);
		} else {
			generateLoadObject(buffer, Handles.nil->raw, var->reg, 1);
			var->flags |= VAR_IN_REG;
			asmPush(buffer, var->reg);
		}
		break;

	case OPERAND_SUPER: {
		Variable *self = variableAt(generator, SELF_INDEX);
		fillVar(generator, self);
		asmPush(buffer, varReg(self));
		break;
	}

	case OPERAND_CONTEXT_VAR: {
		Variable *context = specialVariableAt(generator, VAR_CONTEXT, operand.level);
		ptrdiff_t offset = varOffset(RawContext, vars) + operand.index * sizeof(Value);
		fillContext(generator, operand.level);
		asmLdT(buffer, TMP, offset, varReg(context));
		asmPush(buffer, TMP);
		break;
	}

	case OPERAND_INST_VAR: {
		Variable *self = variableAt(generator, SELF_INDEX);
		InstanceShape shape = classGetInstanceShape(generator->code.ownerClass);
		ptrdiff_t offset = varOffset(RawObject, body) + (shape.payloadSize + operand.index + shape.isIndexed) * sizeof(Value);
		fillVar(generator, self);
		asmLdT(buffer, TMP, offset, varReg(self));
		asmPush(buffer, TMP);
		break;
	}

	case OPERAND_LITERAL:
		generateLoadObject(buffer, compiledCodeLiteralAt(&generator->code, operand.index), TMP, 1);
		asmPush(buffer, TMP);
		break;

	case OPERAND_ASSOC:;
		generateLoadObject(buffer, compiledCodeLiteralAt(&generator->code, operand.index), TMP, 1);
		asmLdT(buffer, TMP, varOffset(RawAssociation, value), TMP);
		asmPush(buffer, TMP);
		break;

	case OPERAND_BLOCK:;
		generateLoadBlock(generator, operand);
		asmPush(buffer, R3);
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
		emitLoadImm(buffer, reg, operand.value);
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
		asmLdT(buffer, reg, offset, varReg(context));
		break;
	}

	case OPERAND_INST_VAR: {
		Variable *self = variableAt(generator, SELF_INDEX);
		InstanceShape shape = classGetInstanceShape(generator->code.ownerClass);
		ptrdiff_t offset = varOffset(RawObject, body) + (shape.payloadSize + operand.index + shape.isIndexed) * sizeof(Value);
		fillVar(generator, self);
		asmLdT(buffer, reg, offset, varReg(self));
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
			src = varReg(variable);
		}
		asmLdT(buffer, reg, varOffset(RawAssociation, value), src);
		break;
	}

	case OPERAND_BLOCK:
		generateLoadBlock(generator, operand);
		asmMr(buffer, reg, R3);
		break;

	default:
		FAIL();
	}
}


static _Bool operandMayBePointer(Operand operand)
{
	switch (operand.type) {
	case OPERAND_NIL:
	case OPERAND_TRUE:
	case OPERAND_FALSE:
		return 0;
	case OPERAND_VALUE:
		return valueTypeOf(operand.value, VALUE_POINTER);
	default:
		return 1;
	}
}

static void movToOperand(CodeGenerator *generator, Register reg, Operand operand, _Bool valueMayBePointer)
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
		if (valueMayBePointer) {
			generateStoreCheck(generator, varReg(context), reg);
		}
		asmStdT(buffer, reg, offset, varReg(context));
		break;
	}

	case OPERAND_INST_VAR: {
		Variable *self = variableAt(generator, SELF_INDEX);
		InstanceShape shape = classGetInstanceShape(generator->code.ownerClass);
		ptrdiff_t offset = varOffset(RawObject, body) + (shape.payloadSize + operand.index + shape.isIndexed) * sizeof(Value);

		fillVar(generator, self);
		if (valueMayBePointer) {
			generateStoreCheck(generator, varReg(self), reg);
		}
		asmStdT(buffer, reg, offset, varReg(self));
		break;
	}

	case OPERAND_ASSOC: {
		Variable *variable = specialVariableAt(generator, VAR_ASSOC, operand.index);
		fillAssoc(generator, operand.index);
		asmStdT(buffer, reg, varOffset(RawAssociation, value), varReg(variable));
		if (valueMayBePointer) {
			generateStoreCheck(generator, varReg(variable), reg);
		}
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
			asmB(buffer, label);
		}
		break;

	case OPERAND_NIL:
		if (class != Handles.UndefinedObject->raw) {
			asmB(buffer, label);
		}
		break;

	case OPERAND_TRUE:
		if (class != Handles.True->raw) {
			asmB(buffer, label);
		}
		break;

	case OPERAND_FALSE:
		if (class != Handles.False->raw) {
			asmB(buffer, label);
		}
		break;

	case OPERAND_THIS_CONTEXT: {
		RawClass *contextClass = (generator->code.isBlock ? Handles.BlockContext : Handles.MethodContext)->raw;
		if (class != contextClass) {
			asmB(buffer, label);
		}
		break;
	}

	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR: {
		Variable *var = variableAt(generator, operand.index);

		if (class == Handles.SmallInteger->raw) {
			fillVar(generator, var);
			asmAndiDot(buffer, R0, varReg(var), 3);
			asmBne(buffer, label);
		} else if (class == Handles.Character->raw) {
			// exact tag 0b10: a lone bit-1 test would accept a VALUE_FLOAT
			// immediate (0b11); single compare + branch, the label takes one
			// forward reference
			fillVar(generator, var);
			asmAndiDot(buffer, R0, varReg(var), 3);
			asmCmpldi(buffer, 0, R0, VALUE_CHAR);
			asmBne(buffer, label);
		} else if (class == Handles.SmallFloat64->raw) {
			fillVar(generator, var);
			asmAndiDot(buffer, R0, varReg(var), 3);
			asmCmpldi(buffer, 0, R0, VALUE_FLOAT);
			asmBne(buffer, label);
		} else {
			fillVar(generator, var);
			generateLoadClass(buffer, varReg(var), R3);
			generateLoadObject(buffer, (RawObject *) class, TMP, 1);
			asmCmpd(buffer, 0, R3, TMP);
			asmBne(buffer, label);
		}
		break;
	}

	case OPERAND_SUPER:
		if (class != (RawClass *) asObject(generator->code.ownerClass->raw->superClass)) {
			asmB(buffer, label);
		}
		break;

	case OPERAND_CONTEXT_VAR: {
		Variable *context = specialVariableAt(generator, VAR_CONTEXT, operand.level);
		ptrdiff_t offset = varOffset(RawContext, vars) + operand.index * sizeof(Value);
		fillContext(generator, operand.level);
		asmLdT(buffer, TMP, offset, varReg(context));

		if (class == Handles.SmallInteger->raw) {
			asmAndiDot(buffer, R0, TMP, 3);
			asmBne(buffer, label);
		} else if (class == Handles.Character->raw) {
			// exact tag 0b10 (see the TEMP_VAR variant)
			asmAndiDot(buffer, R0, TMP, 3);
			asmCmpldi(buffer, 0, R0, VALUE_CHAR);
			asmBne(buffer, label);
		} else if (class == Handles.SmallFloat64->raw) {
			asmAndiDot(buffer, R0, TMP, 3);
			asmCmpldi(buffer, 0, R0, VALUE_FLOAT);
			asmBne(buffer, label);
		} else {
			// compute the class like generateSend does: generateLoadClass is
			// immediate-safe and yields a tagged class, while the old raw
			// class-word load dereferenced whatever non-pointer value was in TMP
			generateLoadClass(buffer, TMP, R3);
			generateLoadObject(buffer, (RawObject *) class, TMP, 1);
			asmCmpd(buffer, 0, R3, TMP);
			asmBne(buffer, label);
		}
		break;
	}

	case OPERAND_INST_VAR: {
		Variable *self = variableAt(generator, SELF_INDEX);
		InstanceShape shape = classGetInstanceShape(generator->code.ownerClass);
		ptrdiff_t offset = varOffset(RawObject, body) + (shape.payloadSize + operand.index + shape.isIndexed) * sizeof(Value);
		fillVar(generator, self);

		asmLdT(buffer, TMP, offset, varReg(self));

		if (class == Handles.SmallInteger->raw) {
			asmAndiDot(buffer, R0, TMP, 3);
			asmBne(buffer, label);
		} else if (class == Handles.Character->raw) {
			// exact tag 0b10 (see the TEMP_VAR variant)
			asmAndiDot(buffer, R0, TMP, 3);
			asmCmpldi(buffer, 0, R0, VALUE_CHAR);
			asmBne(buffer, label);
		} else if (class == Handles.SmallFloat64->raw) {
			asmAndiDot(buffer, R0, TMP, 3);
			asmCmpldi(buffer, 0, R0, VALUE_FLOAT);
			asmBne(buffer, label);
		} else {
			// compute the class like generateSend does: generateLoadClass is
			// immediate-safe and yields a tagged class, while the old raw
			// class-word load dereferenced whatever non-pointer value was in TMP
			generateLoadClass(buffer, TMP, R3);
			generateLoadObject(buffer, (RawObject *) class, TMP, 1);
			asmCmpd(buffer, 0, R3, TMP);
			asmBne(buffer, label);
		}
		break;
	}

	case OPERAND_LITERAL: {
		RawObject *literal = compiledCodeLiteralAt(&generator->code, operand.index);
		if (class != literal->class) {
			asmB(buffer, label);
		}
		break;
	}

	case OPERAND_ASSOC: {
		// Load the association's RUNTIME value and check ITS class (see x64).
		generateLoadObject(buffer, compiledCodeLiteralAt(&generator->code, operand.index), TMP, 1);
		asmLdT(buffer, TMP, varOffset(RawAssociation, value), TMP);

		if (class == Handles.SmallInteger->raw) {
			asmAndiDot(buffer, R0, TMP, 3);
			asmBne(buffer, label);
		} else if (class == Handles.Character->raw) {
			// exact tag 0b10 (see the TEMP_VAR variant)
			asmAndiDot(buffer, R0, TMP, 3);
			asmCmpldi(buffer, 0, R0, VALUE_CHAR);
			asmBne(buffer, label);
		} else if (class == Handles.SmallFloat64->raw) {
			asmAndiDot(buffer, R0, TMP, 3);
			asmCmpldi(buffer, 0, R0, VALUE_FLOAT);
			asmBne(buffer, label);
		} else {
			// compute the class like generateSend does: generateLoadClass is
			// immediate-safe and yields a tagged class, while the old raw
			// class-word load dereferenced whatever non-pointer value was in TMP
			generateLoadClass(buffer, TMP, R3);
			generateLoadObject(buffer, (RawObject *) class, TMP, 1);
			asmCmpd(buffer, 0, R3, TMP);
			asmBne(buffer, label);
		}
		break;
	}

	case OPERAND_BLOCK:
		if (class != Handles.Block->raw) {
			asmB(buffer, label);
		}
		break;

	default:
		FAIL();
	}
}


// r3: block
static void generateLoadBlock(CodeGenerator *generator, Operand operand)
{
	HandleScope scope;
	openHandleScope(&scope);

	CompiledBlock *block = scopeHandle(compiledCodeLiteralAt(&generator->code, operand.index));
	NativeCode *nativeBlock = generateBlockCode(block, generator);
	Variable *context = variableAt(generator, CONTEXT_INDEX);
	AssemblerBuffer *buffer = &generator->buffer;

	// allocate a Block
	generateLoadObject(buffer, (RawObject *) Handles.Block->raw, R4, 0);
	asmLi(buffer, R5, 0);
	generateStubCall(generator, &AllocateStub);
	invalidateRegs(&generator->regsAlloc);

	// setup home context
	fillVar(generator, context);
	if (generator->code.isBlock) {
		asmLdT(buffer, TMP, varOffset(RawContext, home), varReg(context));
		asmStdT(buffer, TMP, varOffset(RawBlock, homeContext), R3);
	} else {
		asmStdT(buffer, varReg(context), varOffset(RawBlock, homeContext), R3);
	}

	// setup native code
	asmLi64(buffer, TMP, (uint64_t) nativeBlock); // TODO (same as x64): reallocation?
	asmStdT(buffer, TMP, varOffset(RawBlock, nativeCode), R3);

	// setup compiled code
	generateLoadObject(buffer, (RawObject *) block->raw, TMP, 1);
	asmStdT(buffer, TMP, varOffset(RawBlock, compiledBlock), R3);

	// setup receiver
	Variable *self = variableAt(generator, SELF_INDEX);
	fillVar(generator, self);
	asmStdT(buffer, varReg(self), varOffset(RawBlock, receiver), R3);

	// setup outer context
	asmStdT(buffer, varReg(context), varOffset(RawBlock, outerContext), R3);

	closeHandleScope(&scope, NULL);
}


static void fillContext(CodeGenerator *generator, uint8_t level)
{
	Variable *context = specialVariableAt(generator, VAR_CONTEXT, level);
	if (context->flags & VAR_IN_REG) {
		// nothing
	} else if (level == 0 && (context->flags & VAR_ON_STACK)) {
		fillVar(generator, context);
	} else {
		Variable *outer = specialVariableAt(generator, VAR_CONTEXT, 0);
		fillVar(generator, outer);
		for (uint8_t i = 0; i < level; i++) {
			asmLdT(&generator->buffer, varReg(context), varOffset(RawContext, outer), varReg(outer));
			outer = context;
		}
		context->flags |= VAR_IN_REG;
		// Never spill an outer (level>0) context, see x64.
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
		generateLoadObject(&generator->buffer, compiledCodeLiteralAt(&generator->code, index), varReg(var), 1);
		var->flags |= VAR_IN_REG;
	}
}


static void fillVar(CodeGenerator *generator, Variable *var)
{
	if ((var->flags & VAR_IN_REG) == 0) {
		ASSERT(var->flags & VAR_ON_STACK);
		if (generator->regsAlloc.frameLess) {
			// The shared RegisterAllocator biases frameless arg offsets by
			// +1 for x64's call-pushed return address; POWER keeps the
			// return address in LR, so arg i really sits at i*8(r1).
			asmLd(&generator->buffer, varReg(var),
				(var->frameOffset - 1) * sizeof(intptr_t), R1);
		} else {
			asmLd(&generator->buffer, varReg(var),
				var->frameOffset * sizeof(intptr_t), FP);
		}
		var->flags |= VAR_IN_REG;
	}
}


static void spillVar(CodeGenerator *generator, Variable *var)
{
	ASSERT(var->flags & VAR_IN_REG);
	ptrdiff_t offset = var->frameOffset * sizeof(intptr_t);
	asmStd(&generator->buffer, varReg(var), offset, FP);
	var->flags |= VAR_ON_STACK;
}


static void movVar(CodeGenerator *generator, Variable *var, Register reg)
{
	if (var->reg == SPILLED_REG && (var->flags & VAR_ON_STACK) != 0) {
		asmLd(&generator->buffer, reg, var->frameOffset * sizeof(intptr_t), FP);
	} else if (var->flags & (VAR_IN_REG | VAR_ON_STACK)) {
		ASSERT(varReg(var) != reg);
		fillVar(generator, var);
		asmMr(&generator->buffer, reg, varReg(var));
	} else {
		generateLoadObject(&generator->buffer, Handles.nil->raw, reg, 1);
	}
}


static void movToVar(CodeGenerator *generator, Register reg, Variable *var)
{
	if (var->reg == SPILLED_REG) {
		var->flags |= VAR_ON_STACK;
		asmStd(&generator->buffer, reg, var->frameOffset * sizeof(intptr_t), FP);
	} else {
		ASSERT(reg != varReg(var));
		var->flags |= VAR_IN_REG;
		asmMr(&generator->buffer, varReg(var), reg);
		spillVar(generator, var);
	}
}


static Variable *variableAt(CodeGenerator *generator, ptrdiff_t index)
{
	return &generator->regsAlloc.vars[index];
}


static Variable *specialVariableAt(CodeGenerator *generator, uint8_t type, ptrdiff_t index)
{
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
		// gcEnd, not end, see the x64 original and worker-reflective-st-bug.
		if (i == CONTEXT_INDEX || (var->frameOffset < 0 && (var->flags & VAR_ON_STACK) && var->start <= generator->bytecodeNumber && (generator->overapproxStackmap || generator->bytecodeNumber <= var->gcEnd))) {
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


// C call from JIT code (ELFv1). The stack shape must mirror x64 EXACTLY,
// because the GC's frame walker resolves each frame's stackmap through
// [tempFP+8]: x64's `call` leaves the return address right above the pushed
// RBP; on POWER the return address lives in LR, so it is PUSHED first ,
// [tempFP+8] = the call-site IC (or the storeIp IP, pushed after it, exactly
// like x64's [rbp+8]=IP / [rbp+16]=retaddr). Stashing LR anywhere else
// breaks every scavenge that happens inside a C call (learned the hard way ,
// stale-pointer corruption at bootstrap). This also keeps LR alive across
// the call for frameless primitives' store checks.
void generateCCall(CodeGenerator *generator, intptr_t cFunction, size_t argsSize, _Bool storeIp)
{
	AssemblerBuffer *buffer = &generator->buffer;
	(void) argsSize;   // args are pre-placed in r3..r5 by the call sites

	asmMflr(buffer, R0);
	asmPush(buffer, R0);                       // call-site return address

	if (storeIp) {
		asmBclNext(buffer);                    // LR = address of the next insn
		generateStackmap(generator);           // ic == that address's offset
		asmMflr(buffer, TMP);
		asmPush(buffer, TMP);
	}

	asmPush(buffer, FP);
	asmMr(buffer, FP, R1);
	asmPush(buffer, CTX);                      // spill current context
	asmRldicr(buffer, R1, R1, 0, 59);          // 16-byte alignment
	// ABI callee frame (Abi.h cCallFrameSize): ELFv1 = 48-byte header +
	// 64-byte param save area (the callee stores its LR/CR in OUR header at
	// 16/8(r1); 40(r1) is the TOC save slot emitCallCFunction uses); ELFv2 =
	// just the 32-byte header with the TOC save at 24(r1), no param save
	// area needed for our prototyped, non-variadic, max-8-argument callees.
	asmStdu(buffer, R1, -(ptrdiff_t) gPpc64Abi->cCallFrameSize, R1);

	// exit frame for the C call, the RUNNING worker's thread (TLS)
	asmLoadTls(buffer, TMP, gCurrentThreadTpoff);
	asmLd(buffer, TMP, offsetof(Thread, stackFramesTail), TMP);
	asmStd(buffer, FP, offsetof(EntryStackFrame, exit), TMP);

	gPpc64Abi->emitCallCFunction(buffer, cFunction);

	asmLd(buffer, CTX, -(ptrdiff_t) sizeof(intptr_t), FP); // restore context
	asmMr(buffer, R1, FP);                     // discard the ABI frame
	asmPop(buffer, FP);
	if (storeIp) {
		asmDropStack(buffer, 1);
	}
	asmPop(buffer, R0);                        // restore the call-site LR
	asmMtlr(buffer, R0);
}


void generateMethodContextAllocation(CodeGenerator *generator, size_t size)
{
	AssemblerBuffer *buffer = &generator->buffer;
	Register reg = CTX;
	ptrdiff_t frameOffset = -2 * (ptrdiff_t) sizeof(intptr_t);
	ptrdiff_t compiledCodeOffset = offsetof(NativeCode, compiledCode) - offsetof(NativeCode, insts);

	// spill parent context
	asmStd(buffer, reg, frameOffset, FP);

	// allocate new context
	generateLoadObject(buffer, (RawObject *) Handles.MethodContext->raw, R4, 0);
	asmLi(buffer, R5, (ptrdiff_t) size);
	generateStubCall(generator, &AllocateStub);

	// setup frame pointer
	asmStdT(buffer, FP, varOffset(RawContext, frame), R3);
	// setup parent context
	asmLd(buffer, reg, frameOffset, FP);
	generateStoreCheck(generator, R3, reg);
	asmStdT(buffer, reg, varOffset(RawContext, parent), R3);
	// load thread from the parent, store into the new context
	asmLdT(buffer, reg, varOffset(RawContext, thread), reg);
	asmStdT(buffer, reg, varOffset(RawContext, thread), R3);
	// load native code entry, then its compiled code (negative offset from insts)
	asmLd(buffer, reg, -(ptrdiff_t) sizeof(intptr_t), FP);
	asmLd(buffer, reg, compiledCodeOffset, reg);
	// tag compiled code
	asmAddi(buffer, reg, reg, 1);
	// setup compiled code within the new context
	generateStoreCheck(generator, R3, reg);
	asmStdT(buffer, reg, varOffset(RawContext, code), R3);
	// move context to its designated register and spill it
	asmMr(buffer, reg, R3);
	asmStd(buffer, reg, frameOffset, FP);
}


void generateBlockContextAllocation(CodeGenerator *generator)
{
	AssemblerBuffer *buffer = &generator->buffer;
	ptrdiff_t ctxSizeOffset = offsetof(RawCompiledBlock, header) + offsetof(CompiledCodeHeader, contextSize) - 1;

	// allocate context (TMP holds the tagged CompiledBlock on entry)
	generateLoadObject(buffer, (RawObject *) Handles.BlockContext->raw, R4, 0);
	asmLbz(buffer, R5, ctxSizeOffset, TMP);
	generateStubCall(generator, &AllocateStub);
	asmMr(buffer, CTX, R3);

	// load parent context
	asmLd(buffer, TMP, -2 * (ptrdiff_t) sizeof(intptr_t), FP);
	// setup parent context
	asmStdT(buffer, TMP, varOffset(RawContext, parent), CTX);
	// setup thread
	asmLdT(buffer, TMP, varOffset(RawContext, thread), TMP);
	asmStdT(buffer, TMP, varOffset(RawContext, thread), CTX);
	// load block (r6, NOT r3, which holds the context; DESIGN.md)
	asmLd(buffer, R6, 2 * sizeof(intptr_t), FP);
	// move home context from block to new context
	asmLdT(buffer, TMP, varOffset(RawBlock, homeContext), R6);
	asmStdT(buffer, TMP, varOffset(RawContext, home), CTX);
	// move compiled block from block to new context
	asmLdT(buffer, TMP, varOffset(RawBlock, compiledBlock), R6);
	asmStdT(buffer, TMP, varOffset(RawContext, code), CTX);
	// move outerContext from block to new context
	asmLdT(buffer, TMP, varOffset(RawBlock, outerContext), R6);
	asmStdT(buffer, TMP, varOffset(RawContext, outer), CTX);
}


void generatePushDummyContext(AssemblerBuffer *buffer)
{
	// load the RUNNING worker's thread from TLS, then push its dummy context
	asmLoadTls(buffer, TMP, gCurrentThreadTpoff);
	asmLd(buffer, R0, offsetof(Thread, context), TMP);
	asmPush(buffer, R0);
}


NativeCode *generateDoesNotUnderstand(String *selector)
{
	heapCodegenLockEnter(CurrentThread.heap); // serialize codegen across worker threads
	size_t argsSize = computeArguments(selector);
	AssemblerBuffer buffer;
	asmInitBuffer(&buffer, 128);

	generateLoadObject(&buffer, (RawObject *) selector->raw, R3, 1);
	asmLi(&buffer, R5, (ptrdiff_t) argsSize);

	// jump to the stub (LR still holds the send's return address)
	asmLi64(&buffer, TGT, (uint64_t) getStubNativeCode(&DoesNotUnderstandStub)->insts);
	asmJumpReg(&buffer, TGT);

	NativeCode *code = buildNativeCodeFromAssembler(&buffer);
	heapCodegenLockLeave(CurrentThread.heap);
	return code;
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
	NativeCode *code = allocateNativeCode(CurrentThread.heap, size,
		buffer->pointersOffsetsSize, buffer->icSitesSize);
	code->compiledCode = NULL;
	code->argsSize = 0;
	code->descriptors = NULL;
	code->stackmaps = NULL;
	code->typeFeedback = NULL;
	code->counter = 0;
	asmBindFixups(buffer, code->insts);
	// Bake each IC site's cell address into the still-private buffer (the li64
	// halfword split goes through targetWriteCodePointer) and start every cell
	// unlinked. The ONLY write to these immediates ever: published code is not
	// patched again, the mutable word is the cell (data, ld-reachable, so the
	// li64 non-atomicity never matters).
	IcCell *cells = nativeCodeIcCells(code);
	for (size_t i = 0; i < buffer->icSitesSize; i++) {
		targetWriteCodePointer(buffer->buffer + buffer->icSites[i], (uint64_t) &cells[i]);
		cells[i].state = &gIcUnlinked;
	}
	gIcStats.sites += buffer->icSitesSize;
	asmCopyBuffer(buffer, code->insts, size);
	asmCopyPointersOffsets(buffer, nativeCodePointersOffsets(code));
	// Single funnel for ALL code creation: publish to instruction fetch
	// (dcbst/sync/icbi/isync via __builtin___clear_cache, REQUIRED on POWER).
	osFlushICache(code->insts, size);
	return code;
}


// Baked-pointer read/patch inside emitted code (jit/TargetCodePatch.h): the
// immediate is SPLIT across the four 16-bit halves of the fixed asmLi64
// shape, no contiguous word exists. Callers flush the icache after writes.
uint64_t targetReadCodePointer(const uint8_t *site)
{
	return asmLi64Read(site);
}


void targetWriteCodePointer(uint8_t *site, uint64_t value)
{
	asmLi64Patch(site, value);
}


