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
#include "core/Assert.h"
#include <stdlib.h>
#include <string.h>

// Register ROLES. The compiler never sees these; they exist so the operations
// below read as intent rather than as encoding.
#define X64_ACCUMULATOR RAX  // the value flowing between load and store
#define X64_SCRATCH RCX      // second operand of a comparison, address scratch
#define X64_FRAME RBP

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
	ASSERT(incoming <= abi->argumentRegisterCount);
	for (uint16_t i = 0; i < incoming; i++) {
		asmMovMemReg(buffer, X64_FRAME, slotOffset(i),
			(Register) abi->argumentRegisters[i]);
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
