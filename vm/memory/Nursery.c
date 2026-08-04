#include "memory/Nursery.h"
#include "core/Assert.h"
#include "os/Os.h"
#include <string.h>


// Arm a semispace: return the first address a young object may occupy, which is
// the base rounded up to the object alignment and then given the generation tag
// (see the invariant at the top of Nursery.h).
static uint8_t *armSemispace(uint8_t *base)
{
	uintptr_t aligned = ((uintptr_t) base + (HEAP_OBJECT_ALIGN - 1))
		& ~(uintptr_t) (HEAP_OBJECT_ALIGN - 1);
	return (uint8_t *) (aligned | NEW_SPACE_TAG);
}


void initNursery(Nursery *nursery, struct Heap *heap, size_t totalBytes)
{
	size_t semiSize = totalBytes / 2;
	semiSize = (semiSize + (HEAP_OBJECT_ALIGN - 1)) & ~(size_t) (HEAP_OBJECT_ALIGN - 1);

	// One mapping for both semispaces, so a single range test decides
	// "is this young" without walking a page list.
	nursery->mappingSize = semiSize * 2 + HEAP_OBJECT_ALIGN;
	nursery->mapping = osPageAlloc(nursery->mappingSize, 0);
	ASSERT(nursery->mapping != NULL);

	nursery->semiSize = semiSize;
	nursery->heap = heap;
	nursery->fromSpace = nursery->mapping;
	nursery->toSpace = nursery->mapping + semiSize;
	nursery->top = armSemispace(nursery->fromSpace);
	nursery->limit = nursery->fromSpace + semiSize;
	nursery->scan = NULL;
	nursery->copyTop = NULL;
	nursery->survivorBytes = 0;
	nursery->promotedBytes = 0;
}


void freeNursery(Nursery *nursery)
{
	if (nursery->mapping != NULL) {
		osPageFree(nursery->mapping, nursery->mappingSize);
		nursery->mapping = NULL;
	}
}


size_t nurseryAvailable(Nursery *nursery)
{
	return nursery->top < nursery->limit
		? (size_t) (nursery->limit - nursery->top)
		: 0;
}


uint8_t *nurseryCarve(Nursery *nursery, size_t bytes)
{
	ASSERT((bytes % HEAP_OBJECT_ALIGN) == 0);
	if (nurseryAvailable(nursery) < bytes) {
		return NULL;
	}
	uint8_t *chunk = nursery->top;
	nursery->top += bytes;
	// The tag is a property of the ADDRESS, so it has to survive every bump.
	// Sizes are multiples of the alignment, which is why it does.
	ASSERT(((uintptr_t) chunk & (HEAP_OBJECT_ALIGN - 1)) == NEW_SPACE_TAG);
	return chunk;
}


_Bool nurseryIncludes(Nursery *nursery, uint8_t *address)
{
	uint8_t *base = nursery->mapping;
	return base <= address && address < base + nursery->mappingSize;
}


uint8_t *nurseryFirstObject(Nursery *nursery)
{
	// The SAME function that armed the space, so the walk and the allocator
	// cannot disagree about where the first object is.
	return armSemispace(nursery->fromSpace);
}


_Bool nurseryInFromSpace(Nursery *nursery, uint8_t *address)
{
	uint8_t *base = nursery->fromSpace;
	return base <= address && address < base + nursery->semiSize;
}
