#ifndef NURSERY_H
#define NURSERY_H

// The young generation: two semispaces, bump allocation through per-mutator
// TLABs, copying collection with one aging step before promotion.
//
// Copying stays (ADR 0005) for one reason: the allocation fast path has to be a
// bump and a limit check, because that is the sequence the JIT inlines and
// because the acceptance criterion is about the allocations that REMAIN after
// escape analysis being cheap.
//
// THE ADDRESS-TAG INVARIANT. Bit 3 of an object's address says which generation
// it is in (core/Object.h): set means young. Objects are 16-aligned, so bit 3 is
// inside the alignment padding and free to carry the tag. Concretely, a young
// object sits at an address congruent to 8 modulo 16, and an old object at 0.
// Every cursor in this file preserves that, and every size is a multiple of 16,
// so the congruence is established once when a semispace is armed and never
// re-derived.

#include "core/Object.h"
#include <stddef.h>

struct Heap;
struct ClassTable;

typedef struct Nursery {
	uint8_t *mapping;   // the whole reservation, two semispaces
	size_t mappingSize;
	uint8_t *fromSpace; // mutators allocate here
	uint8_t *toSpace;   // survivors are copied here during a collection
	uint8_t *top;       // bump cursor inside fromSpace, carries NEW_SPACE_TAG
	uint8_t *limit;     // end of fromSpace
	size_t semiSize;
	struct Heap *heap;
	// Cheney scan cursor, valid only while a collection is running.
	uint8_t *scan;
	uint8_t *copyTop;
	size_t survivorBytes;
	size_t promotedBytes;
} Nursery;

void initNursery(Nursery *nursery, struct Heap *heap, size_t totalBytes);
void freeNursery(Nursery *nursery);
// Carve a TLAB of `bytes` out of the shared young space. NULL when exhausted,
// which is the caller's signal to collect. Callers hold the heap's youngLock.
uint8_t *nurseryCarve(Nursery *nursery, size_t bytes);
// Bytes still uncarved in the current from-space.
size_t nurseryAvailable(Nursery *nursery);
_Bool nurseryIncludes(Nursery *nursery, uint8_t *address);
// True while an address lies in the space being EVACUATED. Used to decide
// whether a pointer needs forwarding; never used to validate a slot.
_Bool nurseryInFromSpace(Nursery *nursery, uint8_t *address);

#endif
