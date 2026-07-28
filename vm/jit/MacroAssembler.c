#include "jit/MacroAssembler.h"
#include "jit/CodeBuffer.h"
#include "jit/Jit.h"
#include "core/Assert.h"
#include <stdlib.h>
#include <string.h>

struct MacroAssembler {
	const MacroAssemblerOps *ops;
	CodeBuffer buffer;
	uint16_t frameSlots;
	uint16_t argumentCount;
	void *state;
};


MacroAssembler *maCreate(const MacroAssemblerOps *ops, uint16_t frameSlots,
	uint16_t argumentCount)
{
	MacroAssembler *assembler = calloc(1, sizeof(MacroAssembler));
	ASSERT(assembler != NULL);
	assembler->ops = ops;
	assembler->frameSlots = frameSlots;
	assembler->argumentCount = argumentCount;
	codeBufferInit(&assembler->buffer);
	if (ops->stateBytes > 0) {
		assembler->state = calloc(1, ops->stateBytes);
		ASSERT(assembler->state != NULL);
	}
	if (ops->begin != NULL) {
		ops->begin(assembler);
	}
	return assembler;
}


void maDestroy(MacroAssembler *assembler)
{
	if (assembler->ops->end != NULL) {
		assembler->ops->end(assembler);
	}
	codeBufferFree(&assembler->buffer);
	free(assembler->state);
	free(assembler);
}


const uint8_t *maBytes(MacroAssembler *assembler, size_t *size)
{
	*size = assembler->buffer.size;
	return assembler->buffer.bytes;
}


uint8_t *maPublish(MacroAssembler *assembler, size_t *size)
{
	// Only the host's own backend may be executed. Emitting for a foreign
	// target is legal and useful (that is what the cross-emission test does),
	// but running those bytes is not, and the difference is worth an assertion
	// rather than a crash with no explanation.
	ASSERT(assembler->ops == maHostBackend());
	*size = assembler->buffer.size;
	uint8_t *entry = codeSpaceAllocate(assembler->buffer.size);
	codeSpacePublish(entry, assembler->buffer.bytes, assembler->buffer.size);
	return entry;
}


size_t maOffset(MacroAssembler *assembler) { return assembler->buffer.size; }
void *maState(MacroAssembler *assembler) { return assembler->state; }
CodeBuffer *maBuffer(MacroAssembler *assembler) { return &assembler->buffer; }
uint16_t maFrameSlots(MacroAssembler *assembler) { return assembler->frameSlots; }
uint16_t maArgumentCount(MacroAssembler *assembler) { return assembler->argumentCount; }
const Abi *maAbi(MacroAssembler *assembler) { return assembler->ops->abi; }


// The one derivation of narrow-or-wide; the reasoning is at the declaration.
//
// TWO CEILINGS, and the lower one decides. The ABI's register set is the
// obvious one. The other is that the narrow convention is entered from C
// through positional entry points and there are only six of them
// (JIT_MAX_NARROW_ARGS), so a method past that is wide even on an ABI with the
// registers to carry it: narrow code nothing can call is not an improvement.
_Bool maUsesWideArguments(MacroAssembler *assembler)
{
	uint32_t incoming = (uint32_t) assembler->argumentCount + 1;
	return incoming > (uint32_t) JIT_MAX_NARROW_ARGS + 1
		|| incoming > assembler->ops->abi->argumentRegisterCount;
}


const MacroAssemblerOps *maBackendNamed(const char *name)
{
	for (const MacroAssemblerOps *const *ops = gMacroAssemblerBackends;
			*ops != NULL; ops++) {
		if (strcmp((*ops)->name, name) == 0) {
			return *ops;
		}
	}
	return NULL;
}


const MacroAssemblerOps *maHostBackend(void)
{
#if defined(__x86_64__)
	return maBackendNamed("x64");
#elif defined(__aarch64__)
	return maBackendNamed("arm64");
#elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
	return maBackendNamed("ppc64le");
#elif defined(__powerpc64__)
	return maBackendNamed("ppc64");
#else
#error "no macro assembler backend for this host - see docs/adr/0009"
#endif
}


// ---- dispatchers -----------------------------------------------------------
//
// One line each, and they exist so the template compiler has no `->ops->` in
// it: reading the compiler should not require knowing that a vtable is there.

void maPrologue(MacroAssembler *a, Value nilValue) { a->ops->prologue(a, nilValue); }
void maEpilogue(MacroAssembler *a, uint16_t slot) { a->ops->epilogue(a, slot); }
void maLoadSlot(MacroAssembler *a, uint16_t slot) { a->ops->loadSlot(a, slot); }
void maLoadAbsolute(MacroAssembler *a, const void *p) { a->ops->loadAbsolute(a, p); }
void maStoreSlot(MacroAssembler *a, uint16_t slot) { a->ops->storeSlot(a, slot); }
void maLoadImmediate(MacroAssembler *a, Value v) { a->ops->loadImmediate(a, v); }
void maLoadField(MacroAssembler *a, uint16_t i) { a->ops->loadField(a, i); }
MaLabel *maNewLabel(MacroAssembler *a) { return a->ops->newLabel(a); }
void maBind(MacroAssembler *a, MaLabel *l) { a->ops->bind(a, l); }
void maJump(MacroAssembler *a, MaLabel *l) { a->ops->jump(a, l); }
void maSafepointPoll(MacroAssembler *a, volatile int *f) { a->ops->safepointPoll(a, f); }


void maStoreField(MacroAssembler *a, uint16_t objectSlot, uint16_t fieldIndex,
	uint16_t valueSlot)
{
	a->ops->storeField(a, objectSlot, fieldIndex, valueSlot);
}


void maBranchIfImmediate(MacroAssembler *a, uint16_t slot, Value value,
	MaCondition condition, MaLabel *label)
{
	a->ops->branchIfImmediate(a, slot, value, condition, label);
}


void maBranchIfTag(MacroAssembler *a, uint16_t slot, MaTagTest test, MaLabel *label)
{
	a->ops->branchIfTag(a, slot, test, label);
}


void maBranchIfNotClass(MacroAssembler *a, uint16_t slot, uint32_t classIndex,
	MaLabel *label)
{
	a->ops->branchIfNotClass(a, slot, classIndex, label);
}


void maCallRuntime3(MacroAssembler *a, MaRuntimeFunction function,
	void *pointerArg, uint16_t slotAddressArg, uint64_t integerArg)
{
	a->ops->callRuntime3(a, function, pointerArg, slotAddressArg, integerArg);
}


void maCallPrimitive(MacroAssembler *a, PrimitiveFunction function, uint64_t argc)
{
	a->ops->callPrimitive(a, function, argc);
}
