#ifndef PPC64_WORD_LE_H
#define PPC64_WORD_LE_H

// The LITTLE-ENDIAN half of the ppc64 backend's ONE compile-time seam: how a
// 32-bit instruction word and the four imm halfwords of an asmLi64 sequence
// lay out in memory. Selected by the single, contained block at the top of
// AssemblerPpc64.h (never include this file directly); everything else in
// the backend is byte-order-neutral or runtime/vtable-bound.
#ifdef PPC64_WORD_ORDER_BOUND
#error "a TU can bind only ONE instruction-word byte order"
#endif
#define PPC64_WORD_ORDER_BOUND LITTLE-ENDIAN

#include "core/Assert.h"
#include <stdint.h>

static inline uint32_t ppcLoadWord(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
		| ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static inline void ppcStoreWord(uint8_t *p, uint32_t word)
{
	p[0] = (uint8_t) word;
	p[1] = (uint8_t) (word >> 8);
	p[2] = (uint8_t) (word >> 16);
	p[3] = (uint8_t) (word >> 24);
}

// Reassemble the 64-bit immediate of an emitted asmLi64 sequence, the read
// half of the targetReadCodePointer/targetWriteCodePointer contract
// (jit/TargetCodePatch.h) that the GC's baked-pointer walks use.
//
// WARNING: DELTA vs ppc64 (BE), function 4. THE most dangerous hand-translation
// of this port (silent wrong code, and the GC patches live pointers). For the
// word of instruction k at byte offset 4k, BE stores [W>>24, W>>16, W>>8, W]
// and LE stores [W, W>>8, W>>16, W>>24], so relative to the BE version:
//   opcode check byte  seq[4k]   -> seq[4k+3]
//   imm16 HIGH byte    seq[4k+2] -> seq[4k+1]
//   imm16 LOW byte     seq[4k+3] -> seq[4k]
// Pinned by the golden's checkLi64Patch() and the binutils oracle.
static inline uint64_t asmLi64Read(const uint8_t *seq)
{
	ASSERT(seq[3] >> 2 == 15 && seq[7] >> 2 == 24 && seq[11] >> 2 == 30
		&& seq[15] >> 2 == 25 && seq[19] >> 2 == 24);
	return ((uint64_t) seq[1] << 56) | ((uint64_t) seq[0] << 48)
		| ((uint64_t) seq[5] << 40) | ((uint64_t) seq[4] << 32)
		| ((uint64_t) seq[13] << 24) | ((uint64_t) seq[12] << 16)
		| ((uint64_t) seq[17] << 8) | (uint64_t) seq[16];
}

// Rewrite the four 16-bit immediate halves of an emitted asmLi64 sequence
// (the sldi word is immutable). This is what a GC pointer-patch loop and
// absolute-address fixups will use; `seq` points at the first byte of the
// 5-word sequence in FINAL (or buffer) memory.
static inline void asmLi64Patch(uint8_t *seq, uint64_t imm)
{
	// Defensive: primary opcodes of lis/ori/rldicr/oris/ori live in the top
	// 6 bits of each word == top 6 bits of its LAST byte in little-endian
	// memory (BE reads them off the first byte instead).
	ASSERT(seq[3] >> 2 == 15 && seq[7] >> 2 == 24 && seq[11] >> 2 == 30
		&& seq[15] >> 2 == 25 && seq[19] >> 2 == 24);
	seq[1] = (uint8_t) (imm >> 56); seq[0] = (uint8_t) (imm >> 48);
	seq[5] = (uint8_t) (imm >> 40); seq[4] = (uint8_t) (imm >> 32);
	seq[13] = (uint8_t) (imm >> 24); seq[12] = (uint8_t) (imm >> 16);
	seq[17] = (uint8_t) (imm >> 8); seq[16] = (uint8_t) imm;
}

#endif
