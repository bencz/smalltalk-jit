#ifndef ENDIAN_VM_H
#define ENDIAN_VM_H

// Byte-order facts and alignment-safe multi-byte accessors.
//
// Principle: this is a JIT, host == target, so IN-MEMORY values are always
// native-endian and need no conversion. What these helpers buy is
// (a) ALIGNMENT safety — a strict-alignment target (or UBSan) traps on
//     `*(int32_t *)p` at an odd offset; memcpy compiles to the same single
//     load/store on x86/ppc64 but is defined behavior everywhere; and
// (b) a single greppable seam for every byte-buffer <-> word crossing, which
//     is exactly the surface a big-endian port must reason about.
//
// Persisted formats (the snapshot image) remain NATIVE-endian and are tagged
// with their byte order in the image header; the loader refuses a foreign-
// endian image loudly. See PORTING.md "endianness".

#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define TARGET_BIG_ENDIAN 1
#else
#define TARGET_BIG_ENDIAN 0
#endif

static inline uint32_t loadU32(const void *p)
{
	uint32_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline int32_t loadI32(const void *p)
{
	int32_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline uint64_t loadU64(const void *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static inline void storeU32(void *p, uint32_t v)
{
	memcpy(p, &v, sizeof(v));
}

static inline void storeI32(void *p, int32_t v)
{
	memcpy(p, &v, sizeof(v));
}

static inline void storeU64(void *p, uint64_t v)
{
	memcpy(p, &v, sizeof(v));
}

#endif
