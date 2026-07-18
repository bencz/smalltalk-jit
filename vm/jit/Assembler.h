#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include "core/Assert.h"
#include "core/Endian.h"
#include "core/Thread.h"
#include "jit/TargetCodePatch.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#define ASM_BUFFER_GAP 64

// Fixup "size" tag for a baked 64-bit immediate that is NOT a contiguous
// 8-byte word in the instruction stream (ppc64's 5-instruction asmLi64,
// 20 bytes): asmBindFixup routes it through targetWriteCodePointer instead
// of a raw store. x64 keeps plain size-8 fixups.
#define ASM_FIXUP_SIZE_CODE_POINTER 20

typedef struct {
	uint8_t argRegsSize;
	uint8_t *argRegs;
	uint8_t regsSize;
	uint8_t *regs;
} AvailableRegs;

typedef struct {
	_Bool isBound;
	_Bool isResolved;
	ptrdiff_t offset;
	size_t size;
} AssemblerLabel;

typedef enum {
	ASM_FIXUP_IP,
} AssemblerFixupType;

typedef struct {
	ptrdiff_t offset;
	size_t size;
	AssemblerFixupType type;
	int64_t value;
} AssemblerFixup;

typedef struct {
	uint8_t *buffer;
	uint8_t *end;
	uint8_t *p;
	ptrdiff_t instOffset;
	AssemblerFixup fixups[8];
	uint8_t fixupsSize;
	// Heap array, grown by doubling: a method's site count is bounded only by
	// its size (a big bootstrap-style method bakes thousands). The GC reaches
	// it THROUGH sitesNode (&pointersOffsets), so growth just reallocs and
	// stores the new pointer: the walk only runs with this mutator parked at a
	// safepoint, never mid-realloc (see CodegenSites in Thread.h).
	uint32_t *pointersOffsets;
	size_t pointersOffsetsSize;
	size_t pointersOffsetsCapacity;
	// Offsets of inline-cache cell-address immediates (same per-arch encoding
	// convention as pointersOffsets: x64 = the imm64, ppc64 = first byte of the
	// li64 shape). Deliberately NOT in pointersOffsets: a cell address is not a
	// heap pointer, so the GC must not forward it, and the in-flight CodegenSites
	// walk must skip the zero placeholders. Patched exactly once, with the cell
	// array's final address, in buildNativeCodeFromAssembler before publication.
	// Same heap-array-with-doubling shape as pointersOffsets (GC never reads it).
	uint32_t *icSites;
	size_t icSitesSize;
	size_t icSitesCapacity;
	CodegenSites sitesNode; // GC visibility of the baked pointers (see Thread.h)
} AssemblerBuffer;

static void asmInitBuffer(AssemblerBuffer *buffer, size_t size);
static void asmFreeBuffer(AssemblerBuffer *buffer);
static void asmCopyBuffer(AssemblerBuffer *buffer, uint8_t *dest, size_t size);
static ptrdiff_t asmOffset(AssemblerBuffer *buffer);
static void asmInitLabel(AssemblerLabel *label);
static void asmEmitLabel32(AssemblerBuffer *buffer, AssemblerLabel *label);
static void asmLabelBind(AssemblerBuffer *buffer, AssemblerLabel *label, ptrdiff_t offset);
static AssemblerFixup *asmEmitFixup(AssemblerBuffer *buffer, AssemblerFixupType type, size_t size, ptrdiff_t offset);
static void asmBindFixups(AssemblerBuffer *buffer, uint8_t *p);
static void asmAddPointerOffset(AssemblerBuffer *buffer, ptrdiff_t offset);
static void asmCopyPointersOffsets(AssemblerBuffer *buffer, uint32_t *dest);
static void asmEmitInt8(AssemblerBuffer *buffer, int8_t v);
static void asmEmitUint8(AssemblerBuffer *buffer, uint8_t v);
static void asmEmitUint16(AssemblerBuffer *buffer, uint16_t v);
static void asmEmitInt32(AssemblerBuffer *buffer, int32_t v);
static void asmEmitUint64(AssemblerBuffer *buffer, uint64_t v);

static void asmEnsureCapacity(AssemblerBuffer *buffer);
static void asmBindFixup(AssemblerBuffer *buffer, AssemblerFixup *fixup, int64_t value);


#define ASM_POINTERS_OFFSETS_INITIAL 64
#define ASM_IC_SITES_INITIAL 32

static void asmInitBuffer(AssemblerBuffer *buffer, size_t size)
{
	buffer->buffer = malloc(size + ASM_BUFFER_GAP);
	buffer->end = buffer->buffer + size;
	buffer->p = buffer->buffer;
	buffer->instOffset = 0;
	buffer->fixupsSize = 0;
	buffer->pointersOffsets = malloc(ASM_POINTERS_OFFSETS_INITIAL * sizeof(uint32_t));
	buffer->pointersOffsetsSize = 0;
	buffer->pointersOffsetsCapacity = ASM_POINTERS_OFFSETS_INITIAL;
	buffer->icSites = malloc(ASM_IC_SITES_INITIAL * sizeof(uint32_t));
	buffer->icSitesSize = 0;
	buffer->icSitesCapacity = ASM_IC_SITES_INITIAL;
	if (buffer->buffer == NULL || buffer->pointersOffsets == NULL || buffer->icSites == NULL) {
		FAIL();
	}
	// Codegen runs GC-active and allocates (stackmaps, descriptors), so a
	// scavenge can move objects whose addresses were already baked in here.
	// Register the pointer sites so the collectors can fix them mid-flight
	// (see CodegenSites in Thread.h). Harmless without a heap (golden tests).
	buffer->sitesNode.insts = &buffer->buffer;
	buffer->sitesNode.offsets = &buffer->pointersOffsets;
	buffer->sitesNode.count = &buffer->pointersOffsetsSize;
	buffer->sitesNode.next = CurrentThread.codegenSites;
	CurrentThread.codegenSites = &buffer->sitesNode;
}


static void asmFreeBuffer(AssemblerBuffer *buffer)
{
	// Unlink from the in-flight list (normally the head: buffers nest LIFO).
	CodegenSites **link = &CurrentThread.codegenSites;
	while (*link != NULL && *link != &buffer->sitesNode) {
		link = &(*link)->next;
	}
	if (*link != NULL) {
		*link = buffer->sitesNode.next;
	}
	free(buffer->buffer);
	free(buffer->pointersOffsets);
	free(buffer->icSites);
}


static void asmCopyBuffer(AssemblerBuffer *buffer, uint8_t *dest, size_t size)
{
	memcpy(dest, buffer->buffer, size);
}


static ptrdiff_t asmOffset(AssemblerBuffer *buffer)
{
	return buffer->p - buffer->buffer;
}


static void asmInitLabel(AssemblerLabel *label)
{
	label->isBound = 0;
	label->isResolved = 0;
}


static void asmEmitLabel32(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	ASSERT(!label->isResolved);
	asmEnsureCapacity(buffer);
	label->isResolved = 1;
	if (label->isBound) {
		asmEmitInt32(buffer, label->offset - asmOffset(buffer) - sizeof(int32_t));
	} else {
		label->offset = asmOffset(buffer);
		label->size = sizeof(int32_t);
		asmEmitInt32(buffer, 0);
	}
}


static void asmLabelBind(AssemblerBuffer *buffer, AssemblerLabel *label, ptrdiff_t offset)
{
	ASSERT(!label->isBound);
	label->isBound = 1;

	if (label->isResolved) {
		switch (label->size) {
		case 1: {
			uint8_t *p = buffer->buffer + label->offset;
			*p = offset - label->offset - label->size;
			break;
		}
		case 4:
			storeU32(buffer->buffer + label->offset,
				(uint32_t) (offset - label->offset - label->size));
			break;
		case 8:
			storeU64(buffer->buffer + label->offset,
				(uint64_t) (offset - label->offset - label->size));
			break;
		default:
			FAIL();
		}
	} else {
		label->offset = offset;
	}
}


static AssemblerFixup *asmEmitFixup(AssemblerBuffer *buffer, AssemblerFixupType type, size_t size, ptrdiff_t offset)
{
	ASSERT(buffer->fixupsSize < 8);
	AssemblerFixup *fixup = buffer->fixups + buffer->fixupsSize++;
	fixup->type = type;
	fixup->offset = offset;
	fixup->size = size;
	return fixup;
}


static void asmBindFixups(AssemblerBuffer *buffer, uint8_t *p)
{
	AssemblerFixup *fixup;
	for (uint8_t i = 0; i < buffer->fixupsSize; i++) {
		fixup = buffer->fixups + i;
		switch (fixup->type) {
		case ASM_FIXUP_IP:
			asmBindFixup(buffer, fixup, (intptr_t) p + fixup->offset + fixup->value);
			break;
		default:
			FAIL();
		}
	}
}


static void asmBindFixup(AssemblerBuffer *buffer, AssemblerFixup *fixup, int64_t value)
{
	int8_t *p = (int8_t *) buffer->buffer + fixup->offset;
	switch (fixup->size) {
	case 1:
		ASSERT(INT8_MIN <= value && value <= INT8_MAX);
		*p += value;
		break;
	case 4:
		ASSERT(INT32_MIN <= value && value <= INT32_MAX);
		storeI32(p, (int32_t) value);
		break;
	case 8:
		ASSERT(INT64_MIN <= value && value <= INT64_MAX);
		storeU64(p, (uint64_t) value);
		break;
	case ASM_FIXUP_SIZE_CODE_POINTER:
		targetWriteCodePointer((uint8_t *) p, (uint64_t) value);
		break;
	default:
		FAIL();
	}
}


static void asmAddPointerOffset(AssemblerBuffer *buffer, ptrdiff_t offset)
{
	// The offset is a positive position within the method's machine code, stored
	// and read back as a uint32 (see the GC in Scavenger.c/GarbageCollector.c and
	// nativeCodePointersOffsets in CompiledCode.h). uint16 was the old width, and
	// its 64KB ceiling was reachable: a bootstrap-sized method full of arithmetic
	// sends crossed it on ppc64 when the POWER7 GPR<->FPR memory path lengthened
	// the float fast path.
	ASSERT(0 <= offset && offset <= UINT32_MAX);
	if (buffer->pointersOffsetsSize == buffer->pointersOffsetsCapacity) {
		buffer->pointersOffsetsCapacity *= 2;
		uint32_t *grown = realloc(buffer->pointersOffsets,
			buffer->pointersOffsetsCapacity * sizeof(uint32_t));
		if (grown == NULL) {
			FAIL();
		}
		// Publish through the location sitesNode points at; the GC only walks
		// it with this mutator parked, so it always sees the fresh pointer.
		buffer->pointersOffsets = grown;
	}
	buffer->pointersOffsets[buffer->pointersOffsetsSize++] = (uint32_t) offset;
}


static void asmCopyPointersOffsets(AssemblerBuffer *buffer, uint32_t *dest)
{
	memcpy(dest, buffer->pointersOffsets, buffer->pointersOffsetsSize * sizeof(*dest));
}


static void asmAddIcSite(AssemblerBuffer *buffer, ptrdiff_t offset)
{
	ASSERT(0 <= offset && offset <= UINT32_MAX);
	if (buffer->icSitesSize == buffer->icSitesCapacity) {
		buffer->icSitesCapacity *= 2;
		uint32_t *grown = realloc(buffer->icSites, buffer->icSitesCapacity * sizeof(uint32_t));
		if (grown == NULL) {
			FAIL();
		}
		buffer->icSites = grown;
	}
	buffer->icSites[buffer->icSitesSize++] = (uint32_t) offset;
}


static void asmEmitInt8(AssemblerBuffer *buffer, int8_t v)
{
	*(int8_t *) buffer->p = v;
	buffer->p++;
}


static void asmEmitUint8(AssemblerBuffer *buffer, uint8_t v)
{
	*(uint8_t *) buffer->p = v;
	buffer->p++;
}


// memcpy-based (unaligned-safe, see asmEmitInt32): literal/selector indices in
// the bytecode stream are 16-bit and sit at arbitrary offsets.
static void asmEmitUint16(AssemblerBuffer *buffer, uint16_t v)
{
	storeU16(buffer->p, v);
	buffer->p += sizeof(uint16_t);
}


// memcpy-based (see core/Endian.h): same bytes and same single store on the
// hosts we target, but defined behavior at ANY byte offset — bytecode streams
// interleave 1-byte opcodes with these multi-byte fields, so the destination
// is frequently unaligned.
static void asmEmitInt32(AssemblerBuffer *buffer, int32_t v)
{
	storeI32(buffer->p, v);
	buffer->p += sizeof(int32_t);
}


static void asmEmitUint64(AssemblerBuffer *buffer, uint64_t v)
{
	storeU64(buffer->p, v);
	buffer->p += sizeof(uint64_t);
}


static void asmEnsureCapacity(AssemblerBuffer *buffer)
{
	if (buffer->p >= buffer->end) {
		size_t size = (buffer->end - buffer->buffer + ASM_BUFFER_GAP) * 2;
		size_t pos = buffer->p - buffer->buffer;

		buffer->buffer = realloc(buffer->buffer, size);
		if (buffer->buffer == NULL) {
			FAIL();
		}
		buffer->end = buffer->buffer + size - ASM_BUFFER_GAP;
		buffer->p = buffer->buffer + pos;
	}
}

#endif
