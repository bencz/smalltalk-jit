// The x86-64 INSTRUCTION ENCODING half of the backend: the twenty macro
// operations, and nothing else.
//
// Every register name in the JIT lives in this file. Above it, the template
// compiler speaks only of frame slots and branches (ADR 0009).
//
// What is NOT here is the ABI. Which registers carry arguments, what a callee
// must preserve, how much shadow space a caller reserves: all of that lives in
// vm/jit/x64/abi/<abi>/, one file per ABI, because SysV, Win64 and AIX differ
// in those answers while agreeing completely on how to encode a `mov`. Keeping
// them apart is what makes a new ABI a table rather than a second backend.

#include "jit/x64/EmitterX64.h"
#include "jit/x64/AssemblerX64.h"
#include "jit/InlineCache.h"
#include "jit/Jit.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "core/Handle.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Register ROLES. The compiler never sees these; they exist so the operations
// below read as intent rather than as encoding.
#define X64_ACCUMULATOR RAX  // the value flowing between load and store
#define X64_SCRATCH RCX      // second operand of a comparison, address scratch
#define X64_FRAME RBP
// Two more, for the inline cache only. They have to survive from the class
// guard to the call, so they cannot be argument registers under EITHER ABI this
// emitter serves, and they cannot be callee-saved or the sequence would owe a
// save. R10 and R11 are neither under SysV or Win64 -- which is a fact about two
// tables, so x64Send ASSERTS it rather than trusting this comment.
//
// TWO IS ALL THERE IS. RAX, R10 and R11 are the only registers with that
// property, and the accumulator is one of them, which is why the callee's entry
// point rides in the ACCUMULATOR across the argument marshalling instead of in a
// register of its own: marshalling writes argument registers and nothing else,
// so the accumulator is safe there and a third scratch would not exist to find.
#define X64_CELL R11         // the site's IcCell
#define X64_WAY R10          // the candidate way inside it, walked by the probe

// The label pool GROWS. A label already handed out must never move, because
// branches hold pointers to it, so the pool is a vector of pointers and each
// label is allocated on its own.
typedef struct {
	X64Label **labels;
	uint32_t count;
	uint32_t capacity;
} X64State;

// The macro in EmitterX64.h names a compile-time constant, so it is CHECKED
// against the real struct rather than trusted. It caught a two-megabyte
// inline label pool.
_Static_assert(sizeof(X64State) <= X64_STATE_BYTES,
	"X64_STATE_BYTES is smaller than the state it must hold");


// The frame contract, in one expression. Slot i is bytecode register i.
static int32_t slotOffset(uint16_t index)
{
	return -(int32_t) ((index + 1) * sizeof(Value));
}


// Does this ABI pass an argument in `reg`? Asked before a call sequence stages
// anything in a register, because "which registers carry arguments" is the one
// fact that differs between ABIs sharing this emitter, and the way it goes wrong
// is silent: the sequence assembles, runs on the host, and passes garbage on the
// ABI that happens to use that register.
static _Bool abiUsesAsArgument(const Abi *abi, Register reg)
{
	for (uint8_t i = 0; i < abi->argumentRegisterCount; i++) {
		if ((Register) abi->argumentRegisters[i] == reg) {
			return 1;
		}
	}
	return 0;
}


static X64State *state(MacroAssembler *assembler)
{
	return (X64State *) maState(assembler);
}


// ---- operations ------------------------------------------------------------

void x64Prologue(MacroAssembler *assembler, Value nilValue)
{
	CodeBuffer *buffer = maBuffer(assembler);
	const Abi *abi = maAbi(assembler);
	uint16_t slots = maFrameSlots(assembler);

	asmPush(buffer, X64_FRAME);
	asmMovRegReg(buffer, X64_FRAME, RSP);
	// The return address plus the saved frame pointer is already one alignment
	// unit, so the frame itself must be a whole number of them for the stack to
	// be aligned at any call this method makes.
	uint16_t unit = (uint16_t) (abi->stackAlignment / sizeof(Value));
	int32_t frameBytes = (int32_t) (((slots + unit - 1) / unit) * unit * sizeof(Value));
	asmSubRegImm32(buffer, RSP, frameBytes);

	uint16_t incoming = (uint16_t) (maArgumentCount(assembler) + 1);
	if (!maUsesWideArguments(assembler)) {
		for (uint16_t i = 0; i < incoming; i++) {
			asmMovMemReg(buffer, X64_FRAME, slotOffset(i),
				(Register) abi->argumentRegisters[i]);
		}
	} else {
		// THE WIDE CONVENTION (jit/Jit.h): argument register 0 holds a pointer
		// to the caller's receiver slot, and the arguments are at descending
		// addresses from it -- the same order this frame wants them in, so it is
		// a straight copy down rather than the ABI's stack-argument area being
		// staged by the caller and read back here.
		//
		// The accumulator is the only scratch used, and the one way this goes
		// wrong is it BEING the register the block pointer arrived in, which
		// would clobber the pointer on the first copy. That is exactly the
		// mistake abiUsesAsArgument exists for, and it is checked rather than
		// reasoned about: RAX is not an argument register under SysV or Win64,
		// but that is a fact about two tables, not about this code.
		Register block = (Register) abi->argumentRegisters[0];
		ASSERT(!abiUsesAsArgument(abi, X64_ACCUMULATOR));
		for (uint16_t i = 0; i < incoming; i++) {
			asmMovRegMem(buffer, X64_ACCUMULATOR, block,
				-(int32_t) ((uint32_t) i * sizeof(Value)));
			asmMovMemReg(buffer, X64_FRAME, slotOffset(i), X64_ACCUMULATOR);
		}
	}

	// Every remaining slot starts as nil. Not tidiness: a slot written on only
	// one arm of a conditional would otherwise read as stack garbage, and the
	// frame map would hand that garbage to the collector as a root.
	if (slots > incoming) {
		asmMovRegImm64(buffer, X64_ACCUMULATOR, nilValue);
		for (uint16_t i = incoming; i < slots; i++) {
			asmMovMemReg(buffer, X64_FRAME, slotOffset(i), X64_ACCUMULATOR);
		}
	}
}


// Tear the frame down and return whatever is already in the result register.
// Shared by the ordinary epilogue and by a primitive that succeeded, so the two
// can never drift into leaving the stack in different states.
static void emitLeaveAndReturn(CodeBuffer *buffer)
{
	asmMovRegReg(buffer, RSP, X64_FRAME);
	asmPop(buffer, X64_FRAME);
	asmRet(buffer);
}


void x64Epilogue(MacroAssembler *assembler, uint16_t resultSlot)
{
	CodeBuffer *buffer = maBuffer(assembler);
	asmMovRegMem(buffer, (Register) maAbi(assembler)->integerResult, X64_FRAME,
		slotOffset(resultSlot));
	emitLeaveAndReturn(buffer);
}


void x64LoadSlot(MacroAssembler *a, uint16_t slot)
{
	asmMovRegMem(maBuffer(a), X64_ACCUMULATOR, X64_FRAME, slotOffset(slot));
}


void x64LoadAbsolute(MacroAssembler *a, const void *address)
{
	CodeBuffer *buffer = maBuffer(a);
	asmMovRegImm64(buffer, X64_ACCUMULATOR, (uint64_t) (uintptr_t) address);
	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_ACCUMULATOR, 0);
}


void x64StoreSlot(MacroAssembler *a, uint16_t slot)
{
	asmMovMemReg(maBuffer(a), X64_FRAME, slotOffset(slot), X64_ACCUMULATOR);
}


void x64LoadImmediate(MacroAssembler *a, Value value)
{
	asmMovRegImm64(maBuffer(a), X64_ACCUMULATOR, value);
}


void x64LoadField(MacroAssembler *a, uint16_t fieldIndex)
{
	// The object is TAGGED, so the field offset is biased by the tag and the
	// load needs no untagging at all.
	asmMovRegMem(maBuffer(a), X64_ACCUMULATOR, X64_ACCUMULATOR,
		(int32_t) (HEADER_SIZE + fieldIndex * sizeof(Value)) - 1);
}


void x64StoreField(MacroAssembler *a, uint16_t objectSlot,
	uint16_t fieldIndex, uint16_t valueSlot)
{
	CodeBuffer *buffer = maBuffer(a);
	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_FRAME, slotOffset(objectSlot));
	asmMovRegMem(buffer, X64_SCRATCH, X64_FRAME, slotOffset(valueSlot));
	asmMovMemReg(buffer, X64_ACCUMULATOR,
		(int32_t) (HEADER_SIZE + fieldIndex * sizeof(Value)) - 1, X64_SCRATCH);
}


MaLabel *x64NewLabel(MacroAssembler *a)
{
	X64State *s = state(a);
	if (s->count == s->capacity) {
		s->capacity = s->capacity == 0 ? 32 : s->capacity * 2;
		s->labels = realloc(s->labels, s->capacity * sizeof(X64Label *));
		ASSERT(s->labels != NULL);
	}
	X64Label *label = malloc(sizeof(X64Label));
	ASSERT(label != NULL);
	asmInitLabel(label);
	s->labels[s->count++] = label;
	return (MaLabel *) label;
}


void x64End(MacroAssembler *a)
{
	X64State *s = state(a);
	for (uint32_t i = 0; i < s->count; i++) {
		free(s->labels[i]);
	}
	free(s->labels);
	s->labels = NULL;
	s->count = s->capacity = 0;
}


void x64Bind(MacroAssembler *a, MaLabel *label)
{
	asmBind(maBuffer(a), (X64Label *) label);
}


void x64Jump(MacroAssembler *a, MaLabel *label)
{
	asmJmp(maBuffer(a), (X64Label *) label);
}


void x64BranchIfImmediate(MacroAssembler *a, uint16_t slot, Value value,
	MaCondition condition, MaLabel *label)
{
	CodeBuffer *buffer = maBuffer(a);
	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_FRAME, slotOffset(slot));
	asmMovRegImm64(buffer, X64_SCRATCH, value);
	asmCmpRegReg(buffer, X64_ACCUMULATOR, X64_SCRATCH);
	asmJcc(buffer, condition == MA_EQUAL ? COND_EQUAL : COND_NOT_EQUAL,
		(X64Label *) label);
}


void x64BranchIfTag(MacroAssembler *a, uint16_t slot, MaTagTest test,
	MaLabel *label)
{
	CodeBuffer *buffer = maBuffer(a);
	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_FRAME, slotOffset(slot));
	asmTestRegImm32(buffer, X64_ACCUMULATOR, 3);
	switch (test) {
	case MA_TAG_SMALLINT:
		asmJcc(buffer, COND_EQUAL, (X64Label *) label);
		break;
	case MA_TAG_NOT_POINTER:
		// Tag 01 is a pointer; anything else is not.
		asmJcc(buffer, COND_EQUAL, (X64Label *) label);
		break;
	case MA_TAG_POINTER:
		asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) label);
		break;
	}
}


void x64BranchIfNotClass(MacroAssembler *a, uint16_t slot,
	uint32_t classIndex, MaLabel *label)
{
	CodeBuffer *buffer = maBuffer(a);
	// An immediate has no header at all, so it fails first, by its tag.
	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_FRAME, slotOffset(slot));
	asmTestRegImm32(buffer, X64_ACCUMULATOR, 3);
	asmJcc(buffer, COND_EQUAL, (X64Label *) label);
	asmMovRegReg(buffer, X64_SCRATCH, X64_ACCUMULATOR);
	asmSubRegImm32(buffer, X64_SCRATCH, 1); // untag
	// Three instructions, not one: bits 22..31 of the header's low word are
	// identity hash, so the index has to be masked out before comparing. See
	// the correction section of ADR 0005.
	asmMov32RegMem(buffer, X64_ACCUMULATOR, X64_SCRATCH, 0);
	asmAnd32RegImm32(buffer, X64_ACCUMULATOR, (uint32_t) OBJ_CLASS_MASK);
	asmCmp32RegImm32(buffer, X64_ACCUMULATOR, classIndex & (uint32_t) OBJ_CLASS_MASK);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) label);
}


void x64CallRuntime3(MacroAssembler *a, MaRuntimeFunction function,
	void *pointerArg, uint16_t slotAddressArg, uint64_t integerArg)
{
	CodeBuffer *buffer = maBuffer(a);
	const Abi *abi = maAbi(a);
	asmMovRegImm64(buffer, (Register) abi->argumentRegisters[0],
		(uint64_t) (uintptr_t) pointerArg);
	asmLeaRegMem(buffer, (Register) abi->argumentRegisters[1], X64_FRAME,
		slotOffset(slotAddressArg));
	asmMovRegImm64(buffer, (Register) abi->argumentRegisters[2], integerArg);
	if (abi->shadowSpaceBytes != 0) {
		asmSubRegImm32(buffer, RSP, abi->shadowSpaceBytes);
	}
	ASSERT(!abiUsesAsArgument(abi, X64_ACCUMULATOR));
	uint64_t target;
	memcpy(&target, &function, sizeof(target));
	asmMovRegImm64(buffer, X64_ACCUMULATOR, target);
	asmCallReg(buffer, X64_ACCUMULATOR);
	if (abi->shadowSpaceBytes != 0) {
		asmAddRegImm32(buffer, RSP, abi->shadowSpaceBytes);
	}
	// The result is already in the ABI's result register, which is also this
	// backend's accumulator; a backend where they differ moves it here.
}


// ---- the inline cache ------------------------------------------------------

// Byte offsets into the site's cell. Way ZERO is the only one the emitted code
// reads, which is what icPromoteHottest exists to keep true.
#define IC_OFF_SENDS  ((int32_t) offsetof(IcCell, sends))
#define IC_OFF_WAYS   ((int32_t) offsetof(IcCell, ways))
#define IC_OFF_INLINE ((int32_t) offsetof(IcCell, inlineOp))
// And into one way, which a register walks along, so the block that commits to
// a hit is emitted ONCE however many ways are tested.
#define WAY_OFF_CLASS  ((int32_t) offsetof(IcWay, classIndex))
#define WAY_OFF_ARG    ((int32_t) offsetof(IcWay, argClassIndex))
#define WAY_OFF_COUNT  ((int32_t) offsetof(IcWay, count))
#define WAY_OFF_TARGET ((int32_t) offsetof(IcWay, target))
#define WAY_STRIDE     ((int32_t) sizeof(IcWay))


// The class index of the value in `slot`, into X64_SCRATCH, clobbering the
// accumulator.
//
// THE SAME TWO CASES AS classIndexOfValue (core/Class.h), in the same order and
// out of the same table. A pointer answers from the low word of its own header,
// which is the whole point of the class index living there (ADR 0005); anything
// else answers from gClassIndexByTag. Emitting a switch over the three immediate
// tags instead would be a second encoding of a fact the C already owns, and the
// two would drift the first time a tag is added.
// WHICH ARM FALLS THROUGH is the pointer one, and it is worth saying why the
// obvious next step was measured and NOT taken: the pointer arm still ends in a
// `jmp` over the immediate arm, and removing that would mean emitting the
// immediate arm OUT OF LINE, past the end of the whole send sequence. That needs
// deferred-block machinery in the emitter, and what it buys is one correctly
// predicted taken branch. The machinery was written and reverted; if this is
// ever revisited, the reason to do it is instruction FETCH density across the
// whole image, not the branch itself.
static void emitClassIndexOfSlot(MacroAssembler *a, uint16_t slot)
{
	CodeBuffer *buffer = maBuffer(a);
	MaLabel *immediate = x64NewLabel(a);
	MaLabel *done = x64NewLabel(a);

	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_FRAME, slotOffset(slot));
	asmMovRegReg(buffer, X64_SCRATCH, X64_ACCUMULATOR);
	asmAnd32RegImm32(buffer, X64_SCRATCH, 3); // the tag, zero-extended
	asmCmp32RegImm32(buffer, X64_SCRATCH, (uint32_t) VALUE_POINTER);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) immediate);

	// A heap object: untag, then read the class index out of the header's low
	// word. Bits 22..31 there are identity hash, hence the mask.
	asmSubRegImm32(buffer, X64_ACCUMULATOR, (int32_t) VALUE_POINTER);
	asmMov32RegMem(buffer, X64_SCRATCH, X64_ACCUMULATOR, 0);
	asmAnd32RegImm32(buffer, X64_SCRATCH, (uint32_t) OBJ_CLASS_MASK);
	asmJmp(buffer, (X64Label *) done);

	// An immediate: gClassIndexByTag[tag]. The scale is done with a shift and an
	// add rather than a scaled-index addressing mode, because a shift and an add
	// are what every backend this vocabulary targets already has.
	x64Bind(a, immediate);
	asmShlRegImm8(buffer, X64_SCRATCH, 2); // * sizeof(uint32_t)
	asmMovRegImm64(buffer, X64_ACCUMULATOR,
		(uint64_t) (uintptr_t) gClassIndexByTag);
	asmAddRegReg(buffer, X64_ACCUMULATOR, X64_SCRATCH);
	asmMov32RegMem(buffer, X64_SCRATCH, X64_ACCUMULATOR, 0);
	x64Bind(a, done);
}


// A send: the monomorphic inline cache, and the runtime call it falls back to.
//
// WHAT THE FAST PATH IS ALLOWED TO SKIP, and what it is not. It skips the
// lookup, the frame anchor and the C call. It does NOT skip the PROFILE, and
// that is the whole difference between this and the fast path the previous VM
// had: there, arithmetic that hit its fast path bypassed the cache entirely, so
// the profile of an arithmetic site described exactly the executions that had
// FAILED -- inverted, not merely incomplete (ADR 0004, ADR 0006). Here the hit
// bumps the site's count and the way's, and records the argument's class the
// same way icRecord does, so `a + b` still learns what `b` is. Three extra
// memory operations buys a profile the optimizer can believe.
//
// NO FRAME ANCHOR, and that is not an omission. An anchor exists so a collection
// triggered under a C frame can find the compiled frames beneath it; compiled
// code calling compiled code leaves the saved-frame-pointer chain unbroken, so
// the walk from whatever anchor is pushed deeper runs straight through this
// frame. Anything in the callee that can allocate reaches the runtime through a
// call that pushes one.
//
// WHICH CONVENTION THE CALLEE USES IS DECIDED HERE, statically. A send with n
// arguments can only ever reach a method of arity n, and narrow-or-wide is a
// function of the arity and the ABI alone, so the site knows it without a
// runtime test -- and the runtime refuses to install a target that disagrees
// (jit/Jit.c), which is what keeps this from being an assumption.
// ST_NO_INLINE_CACHE=1 kill-switch: emit EXACTLY the pre-cache send sequence,
// which is the runtime call and nothing else. Read once and cached.
//
// Two things need it. A performance claim needs the other side of the A/B, and
// the profile claim needs more than that: the send counters have to come out
// identical with the fast path on and off, and there is no way to check that
// without being able to turn it off.
static _Bool inlineCacheEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_NO_INLINE_CACHE") == NULL;
	}
	return enabled != 0;
}


// ST_NO_INLINE_ARITH=1 kill-switch for the block below, for the same two reasons
// the cache has one: an A/B needs the other side, and the profile claim needs to
// be checkable. A deterministic program under ST_IC_STATS=1 must report the SAME
// send and way totals with this on and off, because the fast path below does the
// same three updates the cache's own hit path does.
static _Bool inlineArithmeticEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_NO_INLINE_ARITH") == NULL;
	}
	return enabled != 0;
}


// The arithmetic the site may do WITHOUT dispatching, ahead of the cache probe.
//
// WHAT THIS IS NOT: it is not the previous VM's arithmetic fast path. That one
// was keyed on the SELECTOR at compile time and, when it hit, skipped the cache
// -- so an arithmetic site's profile described exactly the executions that had
// FAILED (ADR 0004, ADR 0006). Nothing here is decided at compile time except
// "this site takes one argument": WHICH operation, or whether any is allowed at
// all, is a word the runtime wrote into the cell after seeing which method way 0
// resolved to and which primitive that method carries. The compiler still does
// not know that `+` means addition, which is the property both ADRs protect.
//
// AND IT PAYS THE PROFILE. The commit block below bumps the site's total, way
// zero's count and the argument's class, exactly as the cache's hit path does
// twenty lines further down, so `sends == sum of way counts` still holds and the
// optimizer still sees every execution. Three memory operations, the same three.
//
// WHY WAY ZERO NEEDS NO CLASS TEST HERE. `inlineOp` is written only in
// jit/Jit.c, only after icPromoteHottest has settled way 0, and only when way 0
// is the SmallInteger way. Way 0 can change only on a MISS, and a miss runs that
// same code again. So an armed cell means way 0 is the integer way, and the tag
// tests below prove the operands are integers now.
//
// COST AT A SITE THAT IS NOT ARMED: three instructions, and the cell was being
// materialised anyway. That is what buys not having to know at compile time
// which selectors are arithmetic -- the question the ADRs forbid asking.
// The six relational operations, in the order jit/InlineCache.h lists them for
// BOTH groups. ONE table walked twice, because "which comparison" is a fact that
// must not be written down in two places that can drift.
//
// The two condition columns differ and the difference is not cosmetic: an
// integer `cmp` sets the SIGNED flags, and `ucomisd` sets the UNSIGNED ones
// (that is what makes an SSE compare able to report NaN in the parity flag at
// all). Using COND_LESS on float flags would answer by the sign flag, which an
// SSE comparison never writes.
typedef struct {
	uint16_t integerOp;
	uint16_t floatOp;
	Condition integerCondition;
	Condition floatCondition;
} RelationalArm;

static const RelationalArm gRelationalArms[IC_INLINE_RELATIONAL_COUNT] = {
	{ IC_INLINE_INT_LT, IC_INLINE_FLOAT_LT, COND_LESS,          COND_BELOW },
	{ IC_INLINE_INT_GT, IC_INLINE_FLOAT_GT, COND_GREATER,       COND_ABOVE },
	{ IC_INLINE_INT_LE, IC_INLINE_FLOAT_LE, COND_LESS_EQUAL,    COND_BELOW_EQUAL },
	{ IC_INLINE_INT_GE, IC_INLINE_FLOAT_GE, COND_GREATER_EQUAL, COND_ABOVE_EQUAL },
	{ IC_INLINE_INT_EQ, IC_INLINE_FLOAT_EQ, COND_EQUAL,         COND_EQUAL },
	{ IC_INLINE_INT_NE, IC_INLINE_FLOAT_NE, COND_NOT_EQUAL,     COND_NOT_EQUAL },
};


// One relational arm: test whether this site is armed for `op`, and if it is,
// compare and answer a boolean.
//
// THE COMPARISON IS INSIDE THE ARM AND NOT SHARED ABOVE THE CHAIN, and that is
// forced rather than chosen. A shared compare would have its flags destroyed by
// the very next `cmp` in the dispatch chain, which is how the chain asks which
// operation this is. So each arm re-compares -- one instruction -- and only the
// arm that matches ever executes it.
//
// `false` is materialised BEFORE the conditional jump because both booleans are
// 64-bit immediates: there is no conditional move that selects between two of
// those, and `mov` does not disturb the flags the compare just set.
static void emitRelationalArm(MacroAssembler *a, Register opRegister,
	uint16_t op, Condition condition, void (*compare)(MacroAssembler *),
	MaLabel *nan, MaLabel *commit, MaLabel *next)
{
	CodeBuffer *buffer = maBuffer(a);
	asmCmp32RegImm32(buffer, opRegister, op);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	compare(a);
	if (nan != NULL) {
		// NaN FIRST, and it leaves. Every ordered comparison against a NaN is
		// false and `~=` against one is true; the primitive already answers all
		// six correctly, so the cheapest correct thing is to let it.
		asmJcc(buffer, COND_PARITY, (X64Label *) nan);
	}
	asmMovRegImm64(buffer, X64_ACCUMULATOR, tagPtr(Handles.false_.raw));
	asmJcc(buffer, invertCondition(condition), (X64Label *) commit);
	asmMovRegImm64(buffer, X64_ACCUMULATOR, tagPtr(Handles.true_.raw));
	asmJmp(buffer, (X64Label *) commit);
}


static void emitIntegerCompare(MacroAssembler *a)
{
	// On the TAGGED values, which answers what a comparison of the payloads
	// would: both carry the same two low bits, so the shift is order-preserving
	// over the whole signed range.
	asmCmpRegReg(maBuffer(a), X64_ACCUMULATOR, X64_SCRATCH);
}


static void emitFloatCompare(MacroAssembler *a)
{
	asmUcomisd(maBuffer(a), XMM0, XMM1);
}


static void emitInlineArithmetic(MacroAssembler *a, const MaSendSite *site,
	MaLabel *slow, MaLabel *done)
{
	CodeBuffer *buffer = maBuffer(a);
	MaLabel *commit = x64NewLabel(a);
	MaLabel *commitFloat = x64NewLabel(a);
	MaLabel *floating = x64NewLabel(a);
	MaLabel *next;
	uint8_t i;

	// The armed operation, into the way register -- free here, because the cache
	// probe that owns it has not started.
	asmMov32RegMem(buffer, X64_WAY, X64_CELL, IC_OFF_INLINE);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_NONE);
	asmJcc(buffer, COND_EQUAL, (X64Label *) slow);
	// The two groups need different tag tests, so they split before the operands
	// are even loaded. ONE unsigned compare, which is what keeping the integer
	// operations contiguous in the enum buys (jit/InlineCache.h).
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_LAST);
	asmJcc(buffer, COND_ABOVE, (X64Label *) floating);

	// ---- the integer group -------------------------------------------------
	//
	// Both operands SmallInteger, which is tag 00 and therefore a test against
	// the low two bits. TWO tests rather than one on their OR, because the OR
	// needs a fourth register this sequence does not have (the file header
	// explains why there are only three) and both branches are perfectly
	// predicted at a site that is armed.
	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_FRAME, slotOffset(site->receiverSlot));
	asmTestRegImm32(buffer, X64_ACCUMULATOR, 3);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) slow);
	asmMovRegMem(buffer, X64_SCRATCH, X64_FRAME,
		slotOffset((uint16_t) (site->receiverSlot + 1)));
	asmTestRegImm32(buffer, X64_SCRATCH, 3);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) slow);

	// A tagged SmallInteger IS its payload shifted left by two (core/Object.h),
	// so the tags cancel in addition and subtraction and the tagged operands add
	// directly. Overflow of the 62-bit payload is exactly overflow of the 64-bit
	// signed operation, so one `jo` is the whole range check -- and it goes to
	// the slow path, where the method's own Smalltalk builds the LargeInteger.
	//
	// The accumulator is dead on that branch, which is why clobbering it before
	// the test is safe: the cache probe reloads the receiver from its slot.
	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_ADD);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	asmAddRegReg(buffer, X64_ACCUMULATOR, X64_SCRATCH);
	asmJcc(buffer, COND_OVERFLOW, (X64Label *) slow);
	asmJmp(buffer, (X64Label *) commit);

	// THE RELATIONAL SIX GO SECOND, AND THE ORDER OF THESE ARMS IS A MEASURED
	// DECISION rather than a taxonomy. The chain is walked at run time: an arm at
	// position n pays n compare-and-branch pairs before it, and only the arm that
	// matches ever runs. Measured, 20M iterations each: an operation at position 1
	// costs nothing over the empty loop, one at position 6 costs 1.4 ns.
	//
	// A COUNTED LOOP EXECUTES A COMPARISON AND AN ADDITION PER ITERATION, and
	// every `to:do:` and `whileTrue:` in the system is one, which makes `<=` and
	// `<` the two hottest armed operations there are. They were at positions 9 and
	// 10 behind the arithmetic and the bitwise ops, so every loop in the image
	// walked past eight arms it would never take.
	for (i = 0; i < IC_INLINE_RELATIONAL_COUNT; i++) {
		x64Bind(a, next);
		next = x64NewLabel(a);
		emitRelationalArm(a, X64_WAY, gRelationalArms[i].integerOp,
			gRelationalArms[i].integerCondition, emitIntegerCompare, NULL,
			commit, next);
	}

	x64Bind(a, next);
	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_SUB);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	asmSubRegReg(buffer, X64_ACCUMULATOR, X64_SCRATCH);
	asmJcc(buffer, COND_OVERFLOW, (X64Label *) slow);
	asmJmp(buffer, (X64Label *) commit);

	// Multiplication cannot let both tags cancel -- the product would carry four
	// tag bits -- so one side is untagged first. Overflow is again exactly the
	// 64-bit signed one, because the result that fits is the payload product
	// shifted back up by two.
	x64Bind(a, next);
	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_MUL);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	asmSarRegImm8(buffer, X64_ACCUMULATOR, 2);
	asmImulRegReg(buffer, X64_ACCUMULATOR, X64_SCRATCH);
	asmJcc(buffer, COND_OVERFLOW, (X64Label *) slow);
	asmJmp(buffer, (X64Label *) commit);

	// THE CHEAPEST THREE IN THE WHOLE BLOCK. The SmallInteger tag is 00, so it
	// survives and, or and xor untouched and the TAGGED operands combine
	// directly: no untagging, no retagging, and no range check, because a
	// bitwise combination of two 62-bit payloads is always a 62-bit payload.
	// One instruction each, against roughly ninety for the send they replace.
	x64Bind(a, next);
	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_AND);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	asmAndRegReg(buffer, X64_ACCUMULATOR, X64_SCRATCH);
	asmJmp(buffer, (X64Label *) commit);

	x64Bind(a, next);
	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_OR);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	asmOrRegReg(buffer, X64_ACCUMULATOR, X64_SCRATCH);
	asmJmp(buffer, (X64Label *) commit);

	x64Bind(a, next);
	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_XOR);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	asmXorRegReg(buffer, X64_ACCUMULATOR, X64_SCRATCH);
	asmJmp(buffer, (X64Label *) commit);

	// Floor division and modulo, sharing one divide.
	//
	// GUARDED TO NON-NEGATIVE DIVIDEND AND POSITIVE DIVISOR, which is narrower
	// than the operations are, and deliberately: `//` and `\\` FLOOR, the machine
	// divide TRUNCATES, and the two disagree for exactly one operand sign. Inside
	// the guard they agree, so no correction is needed; outside it the send runs
	// and the kernel's own Smalltalk answers, which is also where a zero divisor
	// has to go because ZeroDivide is raised there.
	//
	// AND THE TAGS ARE LEFT ON, which is not a shortcut but the arithmetic:
	// dividing 4a by 4b answers the untagged quotient a/b and a remainder
	// (a mod b)*4 that is ALREADY TAGGED. So modulo costs nothing to convert
	// back and division costs one shift.
	x64Bind(a, next);
	next = x64NewLabel(a);
	MaLabel *divide = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_FLOORDIV);
	asmJcc(buffer, COND_EQUAL, (X64Label *) divide);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_MOD);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	x64Bind(a, divide);
	asmCmpRegImm32(buffer, X64_SCRATCH, 0);
	asmJcc(buffer, COND_LESS_EQUAL, (X64Label *) slow);
	asmCmpRegImm32(buffer, X64_ACCUMULATOR, 0);
	asmJcc(buffer, COND_LESS, (X64Label *) slow);
	// idiv reads RDX:RAX, so the high half has to exist. The dividend is known
	// non-negative here, so cqo writes zero -- but it is the instruction the
	// architecture requires and omitting it divides by whatever RDX held.
	asmCqo(buffer);
	asmIdivReg(buffer, X64_SCRATCH);
	MaLabel *modulo = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_WAY, IC_INLINE_INT_MOD);
	asmJcc(buffer, COND_EQUAL, (X64Label *) modulo);
	asmShlRegImm8(buffer, X64_ACCUMULATOR, 2);
	asmJmp(buffer, (X64Label *) commit);
	x64Bind(a, modulo);
	asmMovRegReg(buffer, X64_ACCUMULATOR, RDX);
	asmJmp(buffer, (X64Label *) commit);

	// Nothing matched, which an armed cell cannot produce -- every value the
	// runtime writes has an arm above. The send is what runs if it ever does.
	x64Bind(a, next);
	asmJmp(buffer, (X64Label *) slow);

	// THE PROFILE, and it is not optional. See the head of this function.
	x64Bind(a, commit);
	asmAddMemImm32(buffer, X64_CELL, IC_OFF_SENDS, 1);
	asmAddMemImm32(buffer, X64_CELL, IC_OFF_WAYS + WAY_OFF_COUNT, 1);
	// The argument's class needs no computing: the tag test above PROVED it is a
	// SmallInteger, and the class of every SmallInteger is one index out of the
	// same table emitClassIndexOfSlot reads.
	asmMovRegImm64(buffer, X64_WAY, gClassIndexByTag[VALUE_INT]);
	asmMov32MemReg(buffer, X64_CELL, IC_OFF_WAYS + WAY_OFF_ARG, X64_WAY);
	asmJmp(buffer, (X64Label *) done);

	// ---- the float group ---------------------------------------------------
	//
	// SmallFloat64 is a ROTATION of the IEEE pattern, biased so the exponents
	// that occur in practice land in 62 bits (core/Object.h). Decoding is
	// therefore real work -- a shift, a bias, a rotate -- and it is paid twice on
	// the way in and once on the way out. That tax is why this block is worth
	// perhaps three times its cost and not thirty: what it removes is the send,
	// the C frame and the primitive's own re-derivation of both classes, but the
	// boxing between one operation and the next stays. Removing THAT is the SSA
	// optimizer's job and cannot be done here, because tier 1's frame law says
	// every bytecode register is a tagged slot (jit/Lir.h).
	//
	// THE DECODE IS SHARED BY ALL TEN OPERATIONS and only the middle differs,
	// which is what keeps the emitted block at one decode rather than one per
	// operation.
	x64Bind(a, floating);

	// Tag 11 on both, tested as two bits rather than as a value, because
	// comparing against 3 would need a fourth register to hold the masked copy.
	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_FRAME, slotOffset(site->receiverSlot));
	asmTestRegImm32(buffer, X64_ACCUMULATOR, 1);
	asmJcc(buffer, COND_EQUAL, (X64Label *) slow);
	asmTestRegImm32(buffer, X64_ACCUMULATOR, 2);
	asmJcc(buffer, COND_EQUAL, (X64Label *) slow);
	asmMovRegMem(buffer, X64_SCRATCH, X64_FRAME,
		slotOffset((uint16_t) (site->receiverSlot + 1)));
	asmTestRegImm32(buffer, X64_SCRATCH, 1);
	asmJcc(buffer, COND_EQUAL, (X64Label *) slow);
	asmTestRegImm32(buffer, X64_SCRATCH, 2);
	asmJcc(buffer, COND_EQUAL, (X64Label *) slow);

	// The bias, which needs a register because it does not fit an imm32. The way
	// register is free from here to the dispatch below, which is why the armed
	// operation is RE-READ there instead of being kept: one reload against a
	// fourth register this sequence does not have.
	asmMovRegImm64(buffer, X64_WAY, SMALLFLOAT_OFFSET);

	// value -> double, twice. `payload <= 1` is the signed-zero pair, which the
	// encoding maps to payloads 0 and 1 instead of biasing; everything else is
	// unbiased and rotated. Exactly floatValueOf, instruction for instruction.
	MaLabel *receiverZero = x64NewLabel(a);
	asmShrRegImm8(buffer, X64_ACCUMULATOR, 2);
	asmCmpRegImm32(buffer, X64_ACCUMULATOR, 1);
	asmJcc(buffer, COND_BELOW_EQUAL, (X64Label *) receiverZero);
	asmAddRegReg(buffer, X64_ACCUMULATOR, X64_WAY);
	x64Bind(a, receiverZero);
	asmRorRegImm8(buffer, X64_ACCUMULATOR, 1);
	asmMovqXmmFromGpr(buffer, XMM0, X64_ACCUMULATOR);

	MaLabel *argumentZero = x64NewLabel(a);
	asmShrRegImm8(buffer, X64_SCRATCH, 2);
	asmCmpRegImm32(buffer, X64_SCRATCH, 1);
	asmJcc(buffer, COND_BELOW_EQUAL, (X64Label *) argumentZero);
	asmAddRegReg(buffer, X64_SCRATCH, X64_WAY);
	x64Bind(a, argumentZero);
	asmRorRegImm8(buffer, X64_SCRATCH, 1);
	asmMovqXmmFromGpr(buffer, XMM1, X64_SCRATCH);

	// WHICH operation, re-read now that both operands are in the float bank and
	// the accumulator is free to hold it. The way register keeps the bias, which
	// the boxing step below still needs.
	MaLabel *box = x64NewLabel(a);
	asmMov32RegMem(buffer, X64_ACCUMULATOR, X64_CELL, IC_OFF_INLINE);

	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_ACCUMULATOR, IC_INLINE_FLOAT_ADD);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	asmAddsd(buffer, XMM0, XMM1);
	asmJmp(buffer, (X64Label *) box);

	// SECOND, for the reason the integer group states at the same place: a loop
	// runs its comparison every iteration. These six also do NOT answer a float,
	// so they leave the boxing path below alone entirely.
	for (i = 0; i < IC_INLINE_RELATIONAL_COUNT; i++) {
		x64Bind(a, next);
		next = x64NewLabel(a);
		emitRelationalArm(a, X64_ACCUMULATOR, gRelationalArms[i].floatOp,
			gRelationalArms[i].floatCondition, emitFloatCompare, slow,
			commitFloat, next);
	}

	x64Bind(a, next);
	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_ACCUMULATOR, IC_INLINE_FLOAT_SUB);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	asmSubsd(buffer, XMM0, XMM1);
	asmJmp(buffer, (X64Label *) box);

	x64Bind(a, next);
	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_ACCUMULATOR, IC_INLINE_FLOAT_MUL);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	asmMulsd(buffer, XMM0, XMM1);
	asmJmp(buffer, (X64Label *) box);

	x64Bind(a, next);
	next = x64NewLabel(a);
	asmCmp32RegImm32(buffer, X64_ACCUMULATOR, IC_INLINE_FLOAT_DIV);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) next);
	// NO ZERO TEST. IEEE division by zero answers an infinity, which is a Float
	// and an answer; it is the SmallInteger division that has to fail, and that
	// one is not in this group.
	asmDivsd(buffer, XMM0, XMM1);
	asmJmp(buffer, (X64Label *) box);

	x64Bind(a, next);
	asmJmp(buffer, (X64Label *) slow);

	// double -> value, the exact inverse, INCLUDING the range refusal. A result
	// outside the SmallFloat64 window needs a BoxedFloat64, which is an
	// allocation, and allocating is what this whole block exists not to do -- so
	// it goes to the send, whose primitive answers by the ordinary path.
	x64Bind(a, box);
	MaLabel *pack = x64NewLabel(a);
	asmMovqGprFromXmm(buffer, X64_ACCUMULATOR, XMM0);
	// The rotation first. It maps +-0.0 to 0 and 1, which is exactly the pair
	// tagFloat special-cases, so the test below is the same test and not a
	// second encoding of it.
	asmRolRegImm8(buffer, X64_ACCUMULATOR, 1);
	asmCmpRegImm32(buffer, X64_ACCUMULATOR, 1);
	asmJcc(buffer, COND_BELOW_EQUAL, (X64Label *) pack);
	asmSubRegReg(buffer, X64_ACCUMULATOR, X64_WAY);
	// payload >= 2. Below that are the two zero payloads, which this arm did not
	// produce, and the wrap-around of the subtraction, which lands far above.
	asmCmpRegImm32(buffer, X64_ACCUMULATOR, 2);
	asmJcc(buffer, COND_BELOW, (X64Label *) slow);
	// payload < 2^62, tested by shifting the two bits the tag needs off the top.
	// The scratch is free: the argument was consumed into XMM1.
	asmMovRegReg(buffer, X64_SCRATCH, X64_ACCUMULATOR);
	asmShrRegImm8(buffer, X64_SCRATCH, 62);
	asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) slow);
	x64Bind(a, pack);
	asmShlRegImm8(buffer, X64_ACCUMULATOR, 2);
	// The low two bits are zero after the shift, so adding the tag IS or-ing it,
	// and every backend has an add-immediate.
	asmAddRegImm32(buffer, X64_ACCUMULATOR, VALUE_FLOAT);

	x64Bind(a, commitFloat);
	asmAddMemImm32(buffer, X64_CELL, IC_OFF_SENDS, 1);
	asmAddMemImm32(buffer, X64_CELL, IC_OFF_WAYS + WAY_OFF_COUNT, 1);
	asmMovRegImm64(buffer, X64_WAY, gClassIndexByTag[VALUE_FLOAT]);
	asmMov32MemReg(buffer, X64_CELL, IC_OFF_WAYS + WAY_OFF_ARG, X64_WAY);
	asmJmp(buffer, (X64Label *) done);
}


void x64Send(MacroAssembler *a, const MaSendSite *site)
{
	CodeBuffer *buffer = maBuffer(a);
	const Abi *abi = maAbi(a);

	if (!inlineCacheEnabled()) {
		x64CallRuntime3(a, site->miss, site->cell, site->receiverSlot,
			site->missInteger);
		return;
	}

	MaLabel *miss = x64NewLabel(a);
	MaLabel *done = x64NewLabel(a);

	// The two registers the sequence carries across the marshalling. Checked,
	// not assumed: an argument register here would be overwritten by the
	// marshalling on one ABI and not the other, which is the silent kind.
	ASSERT(!abiUsesAsArgument(abi, X64_CELL));
	ASSERT(!abiUsesAsArgument(abi, X64_WAY));
	ASSERT(!abiUsesAsArgument(abi, X64_ACCUMULATOR));

	MaLabel *found = x64NewLabel(a);
	asmMovRegImm64(buffer, X64_CELL, (uint64_t) (uintptr_t) site->cell);

	// The one thing decided at COMPILE time about the block below: a binary send.
	// Arithmetic takes one argument, so a site that takes none or several can
	// never be armed and is spared the three instructions. Which operation, and
	// whether any is allowed, remains the runtime's answer.
	if (site->argumentCount == 1 && inlineArithmeticEnabled()) {
		MaLabel *probe = x64NewLabel(a);
		emitInlineArithmetic(a, site, probe, done);
		x64Bind(a, probe);
	}

	emitClassIndexOfSlot(a, site->receiverSlot);

	// Walk the emitted ways, in order, with a REGISTER pointing at the candidate.
	// The pointer is what lets the commit block below be emitted once no matter
	// how many ways are tested; the alternative is that block duplicated per way,
	// and the argument-class sequence inside it is the largest piece of the whole
	// send.
	//
	// The LAST way branches to the miss and the others branch to the hit, so
	// falling off the end of the walk means the last way matched. No jump is
	// emitted for the common case of the first way matching except the one that
	// skips the remaining compares.
	asmLeaRegMem(buffer, X64_WAY, X64_CELL, IC_OFF_WAYS);
	for (uint8_t way = 0; way < IC_EMITTED_WAYS; way++) {
		if (way > 0) {
			asmLeaRegMem(buffer, X64_WAY, X64_WAY, WAY_STRIDE);
		}
		asmMov32RegMem(buffer, X64_ACCUMULATOR, X64_WAY, WAY_OFF_CLASS);
		asmCmpRegReg(buffer, X64_SCRATCH, X64_ACCUMULATOR);
		if (way + 1 == IC_EMITTED_WAYS) {
			asmJcc(buffer, COND_NOT_EQUAL, (X64Label *) miss);
		} else {
			asmJcc(buffer, COND_EQUAL, (X64Label *) found);
		}
	}
	x64Bind(a, found);

	// A way exists for the class before its target does: icRecord creates the
	// way to profile with, and the target is filled in only once the lookup has
	// run. So a null target is an ordinary state and not a broken one, and it
	// means the same thing as a class mismatch -- go to the runtime. It is also
	// what makes an UNUSED way harmless: a zeroed one carries class index 0,
	// which some class really does have, and a null target is what stops that
	// coincidence from being a call.
	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_WAY, WAY_OFF_TARGET);
	asmCmpRegImm32(buffer, X64_ACCUMULATOR, 0);
	asmJcc(buffer, COND_EQUAL, (X64Label *) miss);

	// Committed. The counters move only now, because a miss reaches icRecord and
	// bumping them before the decision would count this send twice.
	asmAddMemImm32(buffer, X64_CELL, IC_OFF_SENDS, 1);
	asmAddMemImm32(buffer, X64_WAY, WAY_OFF_COUNT, 1);
	MaLabel *argumentProfiled = NULL;
	if (site->argumentCount > 0) {
		// THE ARGUMENT'S CLASS IS PROFILED ONLY WHILE THE WAY IS YOUNG, and this
		// is the largest single cost that used to be paid on every execution of
		// every binary send in the system.
		//
		// Deriving a class index is eight or nine instructions: a tag test, and
		// then either a header load and a mask or a shift and a table load. It ran
		// unconditionally, and the only thing it produced was a field the runtime
		// keeps as LAST SEEN -- read by exactly one consumer, jit/Specialize.c,
		// which asks whether this site's argument has a representation worth
		// specializing on and refuses below SPECIALIZE_MIN_SENDS executions
		// anyway. Two instructions now decide whether to pay the other nine.
		//
		// WHAT IS LOST, stated rather than glossed: the field stops meaning "last
		// seen" and starts meaning "seen during the first ARGUMENT_PROFILE_SENDS
		// executions of this way". For a site whose argument class is stable the
		// two are the same answer. For a site that ALTERNATES -- `a + b` seeing
		// SmallInteger and Float by turns -- "last seen" was already an arbitrary
		// sample of a mixed population, so neither reading was ever the truth
		// there; that is what the per-class way counts on the RECEIVER exist for,
		// and they are untouched. For a site that genuinely changes argument class
		// late, the optimizer can now speculate on the old one, its guard fails at
		// run time and the method deoptimizes: a cost on that site, not a wrong
		// answer, because the guard is emitted from the same field it speculated
		// on.
		//
		// A SIXTY-FOUR BIT COMPARE. The counter is a uint64 and testing its low
		// half would restart profiling every time it crossed 2^32.
		argumentProfiled = x64NewLabel(a);
		asmCmpMemImm32(buffer, X64_WAY, WAY_OFF_COUNT, ARGUMENT_PROFILE_SENDS);
		asmJcc(buffer, COND_ABOVE, (X64Label *) argumentProfiled);
		// It clobbers the accumulator, so the target is re-read from
		// the way afterwards rather than parked somewhere across it: one load
		// against a fourth register this sequence would have to find.
		emitClassIndexOfSlot(a, (uint16_t) (site->receiverSlot + 1));
		asmMov32MemReg(buffer, X64_WAY, WAY_OFF_ARG, X64_SCRATCH);
		asmMovRegMem(buffer, X64_ACCUMULATOR, X64_WAY, WAY_OFF_TARGET);
		// AND TESTED AGAIN, because the re-read is a SECOND read of a word another
		// thread may have cleared in between. jitFlushSendCaches nulls every
		// target, it runs whenever any method dictionary changes, and it runs on
		// whichever thread made the change -- so on a shared heap the window
		// between the first test above and this re-read is real. What follows this
		// is `mov acc, [acc + entry]`, so a target nulled inside that window is a
		// load through zero: the send crashes rather than missing.
		//
		// Measured: the multi-worker extend/remove hammer, three workers sending
		// while a fourth replaces the method, segfaulted INSIDE compiled code here.
		// One test, on the path that already re-reads, and only for sites that
		// carry an argument -- a unary send never re-reads and never needed it.
		asmCmpRegImm32(buffer, X64_ACCUMULATOR, 0);
		asmJcc(buffer, COND_EQUAL, (X64Label *) miss);
		// The warm path rejoins HERE, past the re-read and past its test, and it
		// owes neither: the window those two close is opened by the class
		// derivation between the first read and the second, and the warm path does
		// not derive anything. The target it still carries is the one the test
		// above already approved.
		x64Bind(a, argumentProfiled);
	}

	// The entry point, in the ACCUMULATOR, where it survives the marshalling
	// below because that writes argument registers and nothing else.
	asmMovRegMem(buffer, X64_ACCUMULATOR, X64_ACCUMULATOR,
		(int32_t) offsetof(NativeCode, entry));

	if (maWideForArity(abi, site->argumentCount)) {
		// The wide convention wants one pointer to the receiver's slot, and the
		// arguments already descend from it. Nothing is marshalled.
		asmLeaRegMem(buffer, (Register) abi->argumentRegisters[0], X64_FRAME,
			slotOffset(site->receiverSlot));
	} else {
		// Narrow: the receiver and each argument into the ABI's registers, in
		// order. Consecutive bytecode registers are consecutive frame slots at
		// DESCENDING addresses, which is the same walk the wide prologue does.
		for (uint16_t i = 0; i <= site->argumentCount; i++) {
			asmMovRegMem(buffer, (Register) abi->argumentRegisters[i], X64_FRAME,
				slotOffset((uint16_t) (site->receiverSlot + i)));
		}
	}
	// NO SHADOW SPACE, unlike a runtime call: the callee is compiled code, whose
	// prologue moves its argument registers straight into its own frame and
	// never spills them to the caller's. What the callee owes a C function it
	// calls, its own call sequence reserves.
	asmCallReg(buffer, X64_ACCUMULATOR);
	asmJmp(buffer, (X64Label *) done);

	x64Bind(a, miss);
	x64CallRuntime3(a, site->miss, site->cell, site->receiverSlot,
		site->missInteger);
	x64Bind(a, done);
	// Both arms leave the answer in the ABI's result register, which is this
	// backend's accumulator, so the caller's storeSlot needs no help.
}


void x64CallPrimitive(MacroAssembler *a, PrimitiveFunction function, uint64_t argc)
{
	CodeBuffer *buffer = maBuffer(a);
	const Abi *abi = maAbi(a);

	// The primitive's whole argument list is the ADDRESS of the receiver's slot:
	// consecutive bytecode registers are consecutive frame slots, so the
	// receiver and its arguments are already laid out, and nothing is marshalled.
	asmLeaRegMem(buffer, (Register) abi->argumentRegisters[0], X64_FRAME,
		slotOffset(0));
	asmMovRegImm64(buffer, (Register) abi->argumentRegisters[1], argc);
	if (abi->shadowSpaceBytes != 0) {
		asmSubRegImm32(buffer, RSP, abi->shadowSpaceBytes);
	}
	// The target goes in the ACCUMULATOR and not in the scratch, and the
	// difference is not cosmetic: the scratch is RCX, which is SysV's fourth
	// argument register but Win64's FIRST. Staging the address there would
	// overwrite the argument set up two instructions earlier, on Win64 only, in
	// a backend whose whole point is that one compiler serves every ABI.
	ASSERT(!abiUsesAsArgument(abi, X64_ACCUMULATOR));
	uint64_t target;
	memcpy(&target, &function, sizeof(target));
	asmMovRegImm64(buffer, X64_ACCUMULATOR, target);
	asmCallReg(buffer, X64_ACCUMULATOR);
	if (abi->shadowSpaceBytes != 0) {
		asmAddRegImm32(buffer, RSP, abi->shadowSpaceBytes);
	}

	// One compare decides it. PRIMITIVE_FAILED is tagPtr(NULL), which no legal
	// Value can equal, so the failure signal needs no out-parameter and no
	// second register (runtime/Primitive.h).
	MaLabel *failed = x64NewLabel(a);
	asmCmpRegImm32(buffer, (Register) abi->integerResult, (int32_t) PRIMITIVE_FAILED);
	asmJcc(buffer, COND_EQUAL, (X64Label *) failed);
	emitLeaveAndReturn(buffer);
	// Failure lands here, on the first byte of the method's own bytecode, with
	// the frame exactly as the prologue left it.
	x64Bind(a, failed);
}


void x64SafepointPoll(MacroAssembler *a, volatile int *flag)
{
	CodeBuffer *buffer = maBuffer(a);
	MaLabel *pass = x64NewLabel(a);
	asmMovRegImm64(buffer, X64_ACCUMULATOR, (uint64_t) (uintptr_t) flag);
	asmCmpMem32Imm32(buffer, X64_ACCUMULATOR, 0, 0);
	asmJcc(buffer, COND_EQUAL, (X64Label *) pass);
	// PENDING: park in heapGcPoll. Falling through is correct while nothing
	// sets the flag, and the check is emitted now so its cost is measurable
	// before the machinery behind it exists.
	x64Bind(a, pass);
}
