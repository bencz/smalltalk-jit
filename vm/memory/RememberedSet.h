#ifndef REMEMBERED_SET_H
#define REMEMBERED_SET_H

// Old-to-young edge log, appended by the write barrier and drained by the
// nursery collector.
//
// Per MUTATOR, not per heap: the barrier is on the hottest store path in the
// system and must not take a lock. A collection (which is stop-the-world) folds
// every mutator's log into the heap-level set, and an exiting worker splices its
// own in on the way out, so nothing is lost when a thread dies.
//
// The GC_REMEMBERED bit in the object header is what keeps the log from growing
// without bound: an object already in the set is not added again.

#include "core/Object.h"
#include <stdlib.h>

#define REMEMBERED_BLOCK_CAPACITY 1024

typedef struct RememberedSetBlock {
	struct RememberedSetBlock *prev;
	RawObject **current;
	RawObject *objects[REMEMBERED_BLOCK_CAPACITY];
} RememberedSetBlock;

typedef struct {
	RememberedSetBlock *blocks;
} RememberedSet;

void rememberedSetGrow(RememberedSet *set);
void rememberedSetFreeBlocks(RememberedSetBlock *block);
size_t rememberedSetCount(RememberedSet *set);


static inline void initRememberedSet(RememberedSet *set)
{
	set->blocks = NULL;
	rememberedSetGrow(set);
}


// The write barrier's slow path. The fast path (the tests on generation and on
// the GC_REMEMBERED bit) is inlined by the caller, and by the JIT.
static inline void rememberedSetAdd(RememberedSet *set, RawObject *object)
{
	ASSERT(isOldObject(object));
	ASSERT(!rawObjectHasGcBit(object, GC_REMEMBERED));
	RememberedSetBlock *block = set->blocks;
	if (block->current == block->objects + REMEMBERED_BLOCK_CAPACITY) {
		rememberedSetGrow(set);
		block = set->blocks;
	}
	*block->current++ = object;
	rawObjectSetGcBit(object, GC_REMEMBERED);
}

#endif
