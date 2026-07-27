#ifndef MACRO_ASSEMBLER_H
#define MACRO_ASSEMBLER_H

// The architecture-neutral emission vocabulary, dispatched through an OPS
// STRUCT (ADR 0009).
//
// The template compiler calls only these and never names a register. Each
// backend supplies about twenty functions instead of reimplementing the whole
// bytecode-to-machine-code walk. The previous VM put its seam at the raw
// assembler and paid by keeping two copies of a 2800-line code generator that
// drifted apart; most of its catalogued porting bugs were divergences between
// those copies.
//
// WHY A VTABLE AND NOT #ifdef. Link-time selection makes exactly one backend
// exist per binary, and that forecloses the single most valuable porting tool
// this project had: emitting for a FOREIGN target on the current host and
// comparing the bytes against an oracle, with no emulator in the loop. With an
// ops struct, every backend is present in every build and a cross-emission test
// is an ordinary unit test.
//
// The operations are at the granularity of the BYTECODE, not of the machine:
// "load frame slot i", not "mov rax, [rbp-8]". That is what makes them
// implementable on three-operand register machines (POWER, ARM) as naturally as
// on x86.
//
// THE FRAME is part of this contract, because the deopt map depends on it: slot
// i is bytecode register i, at a fixed offset, with no allocation and no
// per-compile variation (ADR 0001).

#include "core/Object.h"
#include "jit/Abi.h"
#include "runtime/Primitive.h"
#include <stddef.h>
#include <stdint.h>

typedef struct MacroAssembler MacroAssembler;

// The shape of every runtime call compiled code makes: an object pointer, the
// ADDRESS of a frame slot, and an integer. A FUNCTION POINTER TYPE and not
// void*, because ISO C does not let those convert, and because a narrow
// vocabulary is deliberate: a general call sequence is where ABI differences
// (shadow space, alignment, argument order) turn into per-backend bugs.
typedef Value (*MaRuntimeFunction)(void *, Value *, uint64_t);
typedef struct MaLabel MaLabel;

typedef enum {
	MA_EQUAL,
	MA_NOT_EQUAL,
} MaCondition;

// Which tag a value carries. Not a machine condition: POWER and ARM spell these
// differently and some need two instructions.
typedef enum {
	MA_TAG_SMALLINT,     // 00
	MA_TAG_POINTER,      // 01
	MA_TAG_NOT_POINTER,
} MaTagTest;

typedef struct MacroAssemblerOps {
	const char *name;      // "x64", "ppc64", "ppc64le", "arm64"
	const Abi *abi;
	size_t stateBytes;     // per-instance backend scratch

	void (*begin)(MacroAssembler *);
	// Release anything the backend allocated beyond its state block.
	void (*end)(MacroAssembler *);
	void (*prologue)(MacroAssembler *, Value nilValue);
	void (*epilogue)(MacroAssembler *, uint16_t resultSlot);

	void (*loadSlot)(MacroAssembler *, uint16_t slot);
	// Read the 64-bit word at a FIXED address. The address is known when the
	// code is emitted and never moves, which is what makes it an immediate: it
	// is a slot the runtime owns, not an object. A global's literal frame is
	// reached this way, because the frame itself is a C struct the collector
	// updates in place.
	void (*loadAbsolute)(MacroAssembler *, const void *address);
	void (*storeSlot)(MacroAssembler *, uint16_t slot);
	void (*loadImmediate)(MacroAssembler *, Value value);
	void (*loadField)(MacroAssembler *, uint16_t fieldIndex);
	void (*storeField)(MacroAssembler *, uint16_t objectSlot,
		uint16_t fieldIndex, uint16_t valueSlot);

	MaLabel *(*newLabel)(MacroAssembler *);
	void (*bind)(MacroAssembler *, MaLabel *);
	void (*jump)(MacroAssembler *, MaLabel *);
	void (*branchIfImmediate)(MacroAssembler *, uint16_t slot, Value value,
		MaCondition condition, MaLabel *);
	void (*branchIfTag)(MacroAssembler *, uint16_t slot, MaTagTest test, MaLabel *);
	void (*branchIfNotClass)(MacroAssembler *, uint16_t slot, uint32_t classIndex,
		MaLabel *);

	void (*callRuntime3)(MacroAssembler *, MaRuntimeFunction function,
		void *pointerArg, uint16_t slotAddressArg, uint64_t integerArg);
	// Attempt a primitive; RETURN its answer if it succeeded, fall through if it
	// failed. One operation and not three, because "try the primitive, otherwise
	// run the method body" is a single act at the bytecode's level of meaning,
	// and folding the return in means no label has to be threaded through the
	// template compiler for a branch that always goes to the very next byte.
	void (*callPrimitive)(MacroAssembler *, PrimitiveFunction function,
		uint64_t argc);
	void (*safepointPoll)(MacroAssembler *, volatile int *flag);
} MacroAssemblerOps;

// Every backend compiled into this build. NULL-terminated, so a cross-emission
// test can iterate all of them regardless of which one is the host.
extern const MacroAssemblerOps *const gMacroAssemblerBackends[];
// The backend matching the machine this binary runs on. The only one whose
// output may be EXECUTED; the others may only be emitted and inspected.
const MacroAssemblerOps *maHostBackend(void);
const MacroAssemblerOps *maBackendNamed(const char *name);

MacroAssembler *maCreate(const MacroAssemblerOps *ops, uint16_t frameSlots,
	uint16_t argumentCount);
void maDestroy(MacroAssembler *assembler);
// The emitted bytes, before they go anywhere executable. A cross-emission test
// reads them here.
const uint8_t *maBytes(MacroAssembler *assembler, size_t *size);
// Copy into executable memory and answer the entry point. Only legal for the
// host backend.
uint8_t *maPublish(MacroAssembler *assembler, size_t *size);
size_t maOffset(MacroAssembler *assembler);

// Backend-private state and buffer, reached by the backends themselves.
void *maState(MacroAssembler *assembler);
struct CodeBuffer *maBuffer(MacroAssembler *assembler);
uint16_t maFrameSlots(MacroAssembler *assembler);
uint16_t maArgumentCount(MacroAssembler *assembler);
const Abi *maAbi(MacroAssembler *assembler);

// ---- the neutral vocabulary, thin dispatchers ------------------------------

void maPrologue(MacroAssembler *assembler, Value nilValue);
void maEpilogue(MacroAssembler *assembler, uint16_t resultSlot);
void maLoadSlot(MacroAssembler *assembler, uint16_t slot);
void maLoadAbsolute(MacroAssembler *assembler, const void *address);
void maStoreSlot(MacroAssembler *assembler, uint16_t slot);
void maLoadImmediate(MacroAssembler *assembler, Value value);
void maLoadField(MacroAssembler *assembler, uint16_t fieldIndex);
void maStoreField(MacroAssembler *assembler, uint16_t objectSlot,
	uint16_t fieldIndex, uint16_t valueSlot);
MaLabel *maNewLabel(MacroAssembler *assembler);
void maBind(MacroAssembler *assembler, MaLabel *label);
void maJump(MacroAssembler *assembler, MaLabel *label);
void maBranchIfImmediate(MacroAssembler *assembler, uint16_t slot, Value value,
	MaCondition condition, MaLabel *label);
void maBranchIfTag(MacroAssembler *assembler, uint16_t slot, MaTagTest test,
	MaLabel *label);
void maBranchIfNotClass(MacroAssembler *assembler, uint16_t slot,
	uint32_t classIndex, MaLabel *label);
void maCallRuntime3(MacroAssembler *assembler, MaRuntimeFunction function,
	void *pointerArg, uint16_t slotAddressArg, uint64_t integerArg);
void maCallPrimitive(MacroAssembler *assembler, PrimitiveFunction function,
	uint64_t argc);
void maSafepointPoll(MacroAssembler *assembler, volatile int *flag);

#endif
