#ifndef PPC64_WORD_BE_H
#define PPC64_WORD_BE_H

// The BIG-ENDIAN half of the ppc64 backend's ONE compile-time seam: how a
// 32-bit instruction word and the four imm halfwords of an asmLi64 sequence
// lay out in memory. Selected by the single, contained block at the top of
// AssemblerPpc64.h (never include this file directly); everything else in
// the backend is byte-order-neutral or runtime/vtable-bound.
#ifdef PPC64_WORD_ORDER_BOUND
#error "a TU can bind only ONE instruction-word byte order"
#endif
#define PPC64_WORD_ORDER_BOUND BIG-ENDIAN

#include "core/Assert.h"
#include <stdint.h>

static inline uint32_t ppcLoadWord(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
		| ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static inline void ppcStoreWord(uint8_t *p, uint32_t word)
{
	p[0] = (uint8_t) (word >> 24);
	p[1] = (uint8_t) (word >> 16);
	p[2] = (uint8_t) (word >> 8);
	p[3] = (uint8_t) word;
}

// Reassemble the 64-bit immediate of an emitted asmLi64 sequence, the read
// half of the targetReadCodePointer/targetWriteCodePointer contract
// (jit/TargetCodePatch.h) that the GC's baked-pointer walks use.
static inline uint64_t asmLi64Read(const uint8_t *seq)
{
	ASSERT(seq[0] >> 2 == 15 && seq[4] >> 2 == 24 && seq[8] >> 2 == 30
		&& seq[12] >> 2 == 25 && seq[16] >> 2 == 24);
	return ((uint64_t) seq[2] << 56) | ((uint64_t) seq[3] << 48)
		| ((uint64_t) seq[6] << 40) | ((uint64_t) seq[7] << 32)
		| ((uint64_t) seq[14] << 24) | ((uint64_t) seq[15] << 16)
		| ((uint64_t) seq[18] << 8) | (uint64_t) seq[19];
}

// Rewrite the four 16-bit immediate halves of an emitted asmLi64 sequence
// (the sldi word is immutable). This is what a GC pointer-patch loop and
// absolute-address fixups will use; `seq` points at the first byte of the
// 5-word sequence in FINAL (or buffer) memory.
static inline void asmLi64Patch(uint8_t *seq, uint64_t imm)
{
	// Defensive: primary opcodes of lis/ori/rldicr/oris/ori live in the top
	// 6 bits of each big-endian word == top 6 bits of its first byte.
	ASSERT(seq[0] >> 2 == 15 && seq[4] >> 2 == 24 && seq[8] >> 2 == 30
		&& seq[12] >> 2 == 25 && seq[16] >> 2 == 24);
	seq[2] = (uint8_t) (imm >> 56); seq[3] = (uint8_t) (imm >> 48);
	seq[6] = (uint8_t) (imm >> 40); seq[7] = (uint8_t) (imm >> 32);
	seq[14] = (uint8_t) (imm >> 24); seq[15] = (uint8_t) (imm >> 16);
	seq[18] = (uint8_t) (imm >> 8); seq[19] = (uint8_t) imm;
}

#endif
