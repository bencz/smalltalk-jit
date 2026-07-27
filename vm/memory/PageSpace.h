#ifndef PAGE_SPACE_H
#define PAGE_SPACE_H

// A non-moving space built from OS page mappings: the old generation, and the
// executable space that holds generated code.
//
// Non-moving is a decision, not an accident (ADR 0005). Once a class is an
// index rather than an address, almost nothing baked into generated code needs
// relocating, and that was the main thing a moving old generation bought. Not
// moving makes `become:` and pinning trivial and leaves the exec space, which
// can never move, using the same allocator as everything else.
//
// A free chunk IS an object, with FORMAT_FREE, so ONE walk strides over live
// and free alike and the sweeper never needs a side table.

#include "core/Object.h"
#include "memory/ObjectWalk.h"
#include <stddef.h>

// Size-segregated bins, indexed by size in words. Anything larger goes in the
// last bin as an unsorted list; first-fit within it.
#define FREE_BIN_COUNT 64
#define FREE_BIN_LARGE (FREE_BIN_COUNT - 1)

typedef struct HeapPage {
	struct HeapPage *next;
	uint8_t *top;   // bump cursor: fresh space in this page begins here
	uint8_t *end;
	size_t size;    // total mapped bytes, this header included
	uint8_t body[];
} HeapPage;

typedef struct {
	HeapPage *pages;
	RawObject *bins[FREE_BIN_COUNT];
	size_t pageSize;   // default mapping size; a big object gets its own page
	size_t capacity;   // total bytes mapped
	size_t allocated;  // bytes currently handed out as live objects
	_Bool executable;
} PageSpace;

// Iterates every object of the space, live and free, in address order.
typedef struct {
	HeapPage *page;
	uint8_t *cursor;
	ClassTable *classes;
} PageSpaceIterator;

void initPageSpace(PageSpace *space, size_t pageSize, _Bool executable);
void freePageSpace(PageSpace *space);
// Allocate `bytes` (already aligned by the caller through objectAlignSize).
// Returns NULL only if the OS refuses more memory.
uint8_t *pageSpaceAllocate(PageSpace *space, size_t bytes);
// Return a region to the free list, coalescing is left to the sweeper.
void pageSpaceFree(PageSpace *space, uint8_t *address, size_t bytes);
// Rebuild the free lists from the current mark bits, clearing them as it goes.
// Returns bytes reclaimed.
size_t pageSpaceSweep(PageSpace *space, ClassTable *classes);
_Bool pageSpaceIncludes(PageSpace *space, uint8_t *address);

void pageSpaceIteratorInit(PageSpaceIterator *iterator, PageSpace *space,
	ClassTable *classes);
RawObject *pageSpaceIteratorNext(PageSpaceIterator *iterator);


// ---- free chunks ----------------------------------------------------------
//
// A free chunk is a FORMAT_FREE object. Its size lives in the header when it
// fits (up to SIZE_INLINE_MAX_BYTES) and in body word 1 when it does not, with
// body word 0 always the free-list link. A chunk too big for the header field
// is by construction big enough to hold both words, so there is no minimum-size
// exception to reason about: the smallest chunk is one header plus one link,
// which is exactly HEAP_OBJECT_ALIGN.
//
// This is why the header's own big-size encoding (a word BEFORE the header) is
// not used here: a free chunk appears at whatever address a dead object left
// behind, and there is no guarantee of a spare word in front of it.

static inline size_t freeChunkSize(RawObject *chunk)
{
	ASSERT(rawObjectFormat(chunk) == FORMAT_FREE);
	size_t words = (chunk->header >> OBJ_SIZE_SHIFT) & OBJ_SIZE_MASK;
	return words == SIZE_WORDS_BIG
		? (size_t) ((uint64_t *) chunk->body)[1]
		: words * sizeof(uint64_t);
}


static inline RawObject *freeChunkNext(RawObject *chunk)
{
	ASSERT(rawObjectFormat(chunk) == FORMAT_FREE);
	return *(RawObject **) chunk->body;
}


static inline void freeChunkSetNext(RawObject *chunk, RawObject *next)
{
	*(RawObject **) chunk->body = next;
}


static inline void freeChunkInit(RawObject *chunk, size_t bytes)
{
	ASSERT(bytes >= HEAP_OBJECT_ALIGN);
	ASSERT((bytes % HEAP_OBJECT_ALIGN) == 0);
	size_t words = bytes / sizeof(uint64_t);
	// CLASS_INDEX_INVALID: a free chunk names no class, and reading one out of
	// it must fail an assertion rather than land on a real class.
	chunk->header = makeObjectHeader(0, 0, FORMAT_FREE,
		words >= SIZE_WORDS_BIG ? SIZE_WORDS_BIG : words);
	*(RawObject **) chunk->body = NULL;
	if (words >= SIZE_WORDS_BIG) {
		((uint64_t *) chunk->body)[1] = bytes;
	}
}


// Stride for a heap walk: the one place that knows a free chunk sizes itself
// differently from a live object.
static inline size_t heapWalkStride(ClassTable *classes, RawObject *object)
{
	return rawObjectFormat(object) == FORMAT_FREE
		? freeChunkSize(object)
		: objectSizeInBytes(classes, object);
}

#endif
