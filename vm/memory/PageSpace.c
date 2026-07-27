#include "memory/PageSpace.h"
#include "core/Assert.h"
#include "os/Os.h"
#include <string.h>


void initPageSpace(PageSpace *space, size_t pageSize, _Bool executable)
{
	space->pages = NULL;
	memset(space->bins, 0, sizeof(space->bins));
	space->pageSize = pageSize;
	space->capacity = 0;
	space->allocated = 0;
	space->executable = executable;
}


void freePageSpace(PageSpace *space)
{
	HeapPage *page = space->pages;
	while (page != NULL) {
		HeapPage *next = page->next;
		osPageFree(page, page->size);
		page = next;
	}
	space->pages = NULL;
	space->capacity = 0;
	space->allocated = 0;
}


static size_t binForSize(size_t bytes)
{
	size_t words = bytes / sizeof(uint64_t);
	return words < FREE_BIN_LARGE ? words : FREE_BIN_LARGE;
}


// Push a region onto the free list. The caller guarantees alignment and a size
// of at least HEAP_OBJECT_ALIGN.
void pageSpaceFree(PageSpace *space, uint8_t *address, size_t bytes)
{
	ASSERT(bytes >= HEAP_OBJECT_ALIGN);
	RawObject *chunk = (RawObject *) address;
	freeChunkInit(chunk, bytes);
	size_t bin = binForSize(bytes);
	freeChunkSetNext(chunk, space->bins[bin]);
	space->bins[bin] = chunk;
}


// Split a chunk, returning the tail to the free list when the remainder is
// worth keeping. A remainder smaller than one aligned object cannot become a
// chunk (it has no room for a header plus a link), so it stays with the
// allocation as slack rather than becoming unreachable memory the walk would
// stride straight past.
static uint8_t *carve(PageSpace *space, RawObject *chunk, size_t chunkBytes, size_t bytes)
{
	size_t remainder = chunkBytes - bytes;
	if (remainder >= HEAP_OBJECT_ALIGN) {
		pageSpaceFree(space, (uint8_t *) chunk + bytes, remainder);
	} else {
		bytes = chunkBytes; // slack: the object owns the whole chunk
	}
	space->allocated += bytes;
	return (uint8_t *) chunk;
}


static uint8_t *allocateFromBins(PageSpace *space, size_t bytes)
{
	// Exact-fit bins first: an allocation whose size has its own bin takes the
	// head with no search and no split.
	size_t bin = binForSize(bytes);
	if (bin < FREE_BIN_LARGE && space->bins[bin] != NULL) {
		RawObject *chunk = space->bins[bin];
		space->bins[bin] = freeChunkNext(chunk);
		return carve(space, chunk, freeChunkSize(chunk), bytes);
	}
	// Then any larger exact bin, smallest first.
	for (size_t b = bin + 1; b < FREE_BIN_LARGE; b++) {
		if (space->bins[b] != NULL) {
			RawObject *chunk = space->bins[b];
			space->bins[b] = freeChunkNext(chunk);
			return carve(space, chunk, freeChunkSize(chunk), bytes);
		}
	}
	// Finally first-fit in the large bin.
	RawObject **link = &space->bins[FREE_BIN_LARGE];
	while (*link != NULL) {
		RawObject *chunk = *link;
		size_t size = freeChunkSize(chunk);
		if (size >= bytes) {
			*link = freeChunkNext(chunk);
			return carve(space, chunk, size, bytes);
		}
		link = (RawObject **) chunk->body;
	}
	return NULL;
}


static HeapPage *addPage(PageSpace *space, size_t needed)
{
	size_t size = space->pageSize;
	size_t required = needed + sizeof(HeapPage) + HEAP_OBJECT_ALIGN;
	while (size < required) {
		size *= 2;
	}
	HeapPage *page = osPageAlloc(size, space->executable);
	if (page == NULL) {
		return NULL;
	}
	// osPageAlloc promises zeroed memory, which the object initializers below
	// depend on; do not weaken that contract.
	page->size = size;
	page->next = space->pages;
	// Objects must be 16-aligned AND, because bit 3 of the address is the
	// generation tag (core/Object.h), an old-space object must have it clear.
	// The mapping is page-aligned so the body is too once rounded.
	uintptr_t body = ((uintptr_t) page->body + (HEAP_OBJECT_ALIGN - 1))
		& ~(uintptr_t) (HEAP_OBJECT_ALIGN - 1);
	page->top = (uint8_t *) body;
	page->end = (uint8_t *) page + size;
	space->pages = page;
	space->capacity += size;
	return page;
}


uint8_t *pageSpaceAllocate(PageSpace *space, size_t bytes)
{
	ASSERT(bytes >= HEAP_OBJECT_ALIGN);
	ASSERT((bytes % HEAP_OBJECT_ALIGN) == 0);

	uint8_t *fromBins = allocateFromBins(space, bytes);
	if (fromBins != NULL) {
		return fromBins;
	}
	// Bump in the newest page while it lasts.
	HeapPage *page = space->pages;
	if (page == NULL || (size_t) (page->end - page->top) < bytes) {
		page = addPage(space, bytes);
		if (page == NULL) {
			return NULL;
		}
	}
	uint8_t *address = page->top;
	page->top += bytes;
	space->allocated += bytes;
	return address;
}


_Bool pageSpaceIncludes(PageSpace *space, uint8_t *address)
{
	for (HeapPage *page = space->pages; page != NULL; page = page->next) {
		if ((uint8_t *) page <= address && address < page->end) {
			return 1;
		}
	}
	return 0;
}


void pageSpaceIteratorInit(PageSpaceIterator *iterator, PageSpace *space,
	ClassTable *classes)
{
	iterator->classes = classes;
	iterator->page = space->pages;
	iterator->cursor = NULL;
	if (iterator->page != NULL) {
		uintptr_t body = ((uintptr_t) iterator->page->body + (HEAP_OBJECT_ALIGN - 1))
			& ~(uintptr_t) (HEAP_OBJECT_ALIGN - 1);
		iterator->cursor = (uint8_t *) body;
	}
}


RawObject *pageSpaceIteratorNext(PageSpaceIterator *iterator)
{
	while (iterator->page != NULL) {
		if (iterator->cursor < iterator->page->top) {
			RawObject *object = (RawObject *) iterator->cursor;
			iterator->cursor += heapWalkStride(iterator->classes, object);
			return object;
		}
		iterator->page = iterator->page->next;
		if (iterator->page != NULL) {
			uintptr_t body = ((uintptr_t) iterator->page->body + (HEAP_OBJECT_ALIGN - 1))
				& ~(uintptr_t) (HEAP_OBJECT_ALIGN - 1);
			iterator->cursor = (uint8_t *) body;
		}
	}
	return NULL;
}


// Rebuild the free lists from the mark bits. Adjacent dead objects are
// coalesced into one chunk as the walk goes, which is the cheapest form of
// defragmentation and the reason the sweep runs in address order.
size_t pageSpaceSweep(PageSpace *space, ClassTable *classes)
{
	size_t reclaimed = 0;
	memset(space->bins, 0, sizeof(space->bins));
	space->allocated = 0;

	for (HeapPage *page = space->pages; page != NULL; page = page->next) {
		uintptr_t start = ((uintptr_t) page->body + (HEAP_OBJECT_ALIGN - 1))
			& ~(uintptr_t) (HEAP_OBJECT_ALIGN - 1);
		uint8_t *cursor = (uint8_t *) start;
		uint8_t *deadStart = NULL;

		while (cursor < page->top) {
			RawObject *object = (RawObject *) cursor;
			size_t size = heapWalkStride(classes, object);
			ASSERT(size >= HEAP_OBJECT_ALIGN);
			_Bool live = rawObjectFormat(object) != FORMAT_FREE
				&& rawObjectHasGcBit(object, GC_MARKED);
			if (live) {
				rawObjectClearGcBit(object, GC_MARKED);
				space->allocated += size;
				if (deadStart != NULL) {
					size_t deadSize = (size_t) (cursor - deadStart);
					pageSpaceFree(space, deadStart, deadSize);
					reclaimed += deadSize;
					deadStart = NULL;
				}
			} else if (deadStart == NULL) {
				deadStart = cursor;
			}
			cursor += size;
		}
		if (deadStart != NULL) {
			size_t deadSize = (size_t) (cursor - deadStart);
			pageSpaceFree(space, deadStart, deadSize);
			reclaimed += deadSize;
		}
	}
	return reclaimed;
}
