#include "memory/FreeList.h"
#include "core/Object.h"
#include "memory/HeapPage.h"
#include "memory/Heap.h"
#include <string.h>
#include <stdio.h>

static ptrdiff_t indexForSize(size_t size);
static FreeSpace *popFreeSpace(FreeList *freeList, ptrdiff_t index);
static FreeSpace *popAndSplitFreeSpace(FreeList *freeList, ptrdiff_t index, size_t size);
static FreeSpace *splitFreeSpace(FreeList *freeList, FreeSpace *freeSpace, size_t size);
static ptrdiff_t freeMapNext(uint8_t *freeMap, ptrdiff_t index);
static FreeSpace *createInitialFreeSpace(struct HeapPage *page);
static void freeSpacePrint(FreeSpace *freeSpace);


// Freelist self-check tool (ST_DEBUG_FREELIST=1): validate chunks at every
// add/pop against the owning space's mapped ranges (the FreeList is embedded
// in its PageSpace, so offsetof recovers the owner) and abort WITH A BACKTRACE
// at the guilty call. This caught the freeMapNext out-of-bounds read below by
// flagging the fabricated bin's "chunk" the moment it was popped; costs one
// predictable branch per add/pop when off, nothing on the TLAB fast path.
#include <stdlib.h>
#include <stddef.h>
#include <execinfo.h>

int gFreeListDebug = -1; // set from ST_DEBUG_FREELIST on first scavenge (Scavenger.c)

static void freeListDebugCheck(FreeList *freeList, FreeSpace *freeSpace, const char *op)
{
	PageSpace *space = (PageSpace *) ((char *) freeList - offsetof(PageSpace, freeList));
	if (pageSpaceIncludes(space, (uint8_t *) freeSpace)
			&& freeSpace->size >= HEAP_OBJECT_ALIGN) {
		return;
	}
	uintptr_t *w = (uintptr_t *) freeSpace;
	fprintf(stderr, "FREELIST BAD %s: chunk=%p size=%zu inSpace=%d\n",
		op, (void *) freeSpace, (size_t) freeSpace->size,
		(int) pageSpaceIncludes(space, (uint8_t *) freeSpace));
	fprintf(stderr, "  words: %016zx %016zx %016zx %016zx\n", w[0], w[1], w[2], w[3]);
	void *frames[32];
	int depth = backtrace(frames, 32);
	backtrace_symbols_fd(frames, depth, 2);
	abort();
}

void freeListValidate(FreeList *freeList, PageSpace *space, const char *where)
{
	for (size_t bin = 0; bin <= FREE_LIST_SIZE; bin++) {
		FreeSpace *prev = NULL;
		for (FreeSpace *fs = freeList->freeSpaces[bin]; fs != NULL; fs = fs->next) {
			_Bool bad = !pageSpaceIncludes(space, (uint8_t *) fs)
				|| (fs->tags & TAG_FREESPACE) == 0
				|| fs->size < HEAP_OBJECT_ALIGN;
			if (bad) {
				uintptr_t *w = (uintptr_t *) fs;
				fprintf(stderr, "FREELIST INVALID at %s: bin=%zu chunk=%p inSpace=%d tags=%x size=%zu\n",
					where, bin, (void *) fs, (int) pageSpaceIncludes(space, (uint8_t *) fs),
					(unsigned) fs->tags, (size_t) fs->size);
				if (pageSpaceIncludes(space, (uint8_t *) fs)) {
					fprintf(stderr, "  chunk words: %016zx %016zx %016zx %016zx\n", w[0], w[1], w[2], w[3]);
				}
				if (prev != NULL) {
					uintptr_t *pw = (uintptr_t *) prev;
					fprintf(stderr, "  PREDECESSOR %p words: %016zx %016zx %016zx %016zx %016zx %016zx\n",
						(void *) prev, pw[0], pw[1], pw[2], pw[3], pw[4], pw[5]);
				} else {
					fprintf(stderr, "  chunk was the BIN HEAD (bins array clobbered or bad insert)\n");
				}
				abort();
			}
			prev = fs;
		}
	}
}


void initFreeList(FreeList *freeList, struct HeapPage *page)
{
	for (size_t i = 0; i < FREE_LIST_SIZE; i++) {
		freeList->freeSpaces[i] = NULL;
	}
	freeList->freeSpaces[FREE_LIST_SIZE] = createInitialFreeSpace(page);
	memset(freeList->freeMap, 0, sizeof(freeList->freeMap));
#if FREE_LIST_COLLECT_STATS
	freeList->stats.exactAllocs = 0;
	freeList->stats.nextAllocs = 0;
	freeList->stats.fallbackAllocs = 0;
	freeList->stats.averageSize = 0;
	freeList->stats.addedFreeSpaces = 0;
	freeList->stats.averageAddedSpaceSize = 0;
	freeList->stats.expanded = 0;
#endif
}


void expandFreeList(FreeList *freeList, struct HeapPage *page)
{
#if FREE_LIST_COLLECT_STATS
	freeList->stats.expanded++;
#endif
	freeListAddFreeSpace(freeList, createInitialFreeSpace(page));
}


uint8_t *freeListTryAllocate(FreeList *freeList, size_t size)
{
#if FREE_LIST_COLLECT_STATS
	freeList->stats.averageSize = freeList->stats.averageSize == 0
		? size
		: ((freeList->stats.averageSize + size) / 2);
#endif

	ptrdiff_t index = indexForSize(size);
	if (index != FREE_LIST_SIZE && (freeList->freeMap[index / 8] & (1 << (index % 8))) != 0) {
#if FREE_LIST_COLLECT_STATS
		freeList->stats.exactAllocs++;
#endif
		return (uint8_t *) popFreeSpace(freeList, index);
	}

	if ((index + 1) < FREE_LIST_SIZE) {
		ptrdiff_t nextIndex = freeMapNext(freeList->freeMap, index + 1);
		if (nextIndex != -1) {
			ASSERT(nextIndex > index);
#if FREE_LIST_COLLECT_STATS
			freeList->stats.nextAllocs++;
#endif
			return (uint8_t *) popAndSplitFreeSpace(freeList, nextIndex, size);
		}
	}

	// The oversized bin holds free spaces of DIFFERENT sizes, so we must split
	// the one the scan actually found — not the head of the list — and unlink it
	// in place.
	FreeSpace *prev = NULL;
	FreeSpace *freeSpace = freeList->freeSpaces[FREE_LIST_SIZE];
	while (freeSpace != NULL) {
		if (freeSpace->size >= size) {
#if FREE_LIST_COLLECT_STATS
			freeList->stats.fallbackAllocs++;
#endif
			if (prev == NULL) {
				freeList->freeSpaces[FREE_LIST_SIZE] = freeSpace->next;
			} else {
				prev->next = freeSpace->next;
			}
			if (freeList->freeSpaces[FREE_LIST_SIZE] == NULL) {
				freeList->freeMap[FREE_LIST_SIZE / 8] &= ~(1 << (FREE_LIST_SIZE % 8));
			}
			return (uint8_t *) splitFreeSpace(freeList, freeSpace, size);
		}
		prev = freeSpace;
		freeSpace = freeSpace->next;
	}

	return NULL;
}


static ptrdiff_t indexForSize(size_t size)
{
	ptrdiff_t index = size / HEAP_OBJECT_ALIGN;
	if (index > FREE_LIST_SIZE) {
		index = FREE_LIST_SIZE;
	}
	return index;
}


void freeListAddFreeSpace(FreeList *freeList, FreeSpace *freeSpace)
{
	if (gFreeListDebug > 0) {
		freeListDebugCheck(freeList, freeSpace, "add");
	}
	ptrdiff_t index = indexForSize(freeSpace->size);
	freeSpace->next = freeList->freeSpaces[index];
	freeList->freeSpaces[index] = freeSpace;
	freeList->freeMap[index / 8] |= 1 << (index % 8);
#if FREE_LIST_COLLECT_STATS
	if (index != FREE_LIST_SIZE) {
		freeList->stats.addedFreeSpaces++;
		freeList->stats.averageAddedSpaceSize = freeList->stats.averageAddedSpaceSize == 0
			? freeSpace->size
			: ((freeList->stats.averageAddedSpaceSize + freeSpace->size) / 2);
	}
#endif
}


static FreeSpace *popFreeSpace(FreeList *freeList, ptrdiff_t index)
{
	FreeSpace *result = freeList->freeSpaces[index];
	if (gFreeListDebug > 0) {
		freeListDebugCheck(freeList, result, "pop");
	}
	FreeSpace *next = result->next;
	if (next == NULL) {
		freeList->freeMap[index / 8] &= ~(1 << (index % 8));
	}
	freeList->freeSpaces[index] = next;
	return result;
}


static FreeSpace *popAndSplitFreeSpace(FreeList *freeList, ptrdiff_t index, size_t size)
{
	FreeSpace *freeSpace = popFreeSpace(freeList, index);
	return splitFreeSpace(freeList, freeSpace, size);
}


static FreeSpace *splitFreeSpace(FreeList *freeList, FreeSpace *freeSpace, size_t size)
{
	ASSERT(freeSpace->size >= size);
	size_t newSize = freeSpace->size - size;
	// Exact fit: nothing left over to return to the free list.
	if (newSize == 0) {
		return freeSpace;
	}
	FreeSpace *newFreeSpace = createFreeSpace((uint8_t *) freeSpace + size, newSize);
	freeListAddFreeSpace(freeList, newFreeSpace);
	return freeSpace;
}


static ptrdiff_t freeMapNext(uint8_t *freeMap, ptrdiff_t index)
{
	ASSERT(index < FREE_LIST_SIZE);
	ptrdiff_t element = index / 8;
	// Mask below the start bit WITHIN this byte: the old `1 << index` shifted by
	// the GLOBAL bin number, so for any index >= 8 the mask cleared the whole
	// byte and same-byte candidates above the start bin were silently skipped
	// (falling through to page growth - a waste, not a corruption).
	uint8_t v = freeMap[element] & (uint8_t) ~((1u << (index % 8)) - 1);
	if (v != 0) {
		ptrdiff_t nextIndex = element * 8 + __builtin_ctz(v);
		ASSERT(nextIndex < FREE_LIST_SIZE);
		return nextIndex;
	}

	// Scan the remaining bytes STRICTLY inside the map. The old do-while here
	// incremented before its bounds check, reading freeMap[FREE_MAP_SIZE] - one
	// byte of struct padding past the array. Whenever that byte happened to be
	// non-zero (malloc-history dependent, so image/startup-path dependent), it
	// fabricated a bin >= 136 and freeListTryAllocate then read freeSpaces[]
	// OUT OF BOUNDS - the "chunk" was really a neighboring PageSpace field (an
	// exec page pointer), corrupting the old-space freelist under load.
	for (element++; element < FREE_MAP_SIZE; element++) {
		v = freeMap[element];
		if (v != 0) {
			ptrdiff_t nextIndex = element * 8 + __builtin_ctz(v);
			return nextIndex >= FREE_LIST_SIZE ? -1 : nextIndex;
		}
	}
	return -1;
}


FreeSpace *createFreeSpace(uint8_t *p, size_t size)
{
	ASSERT((uintptr_t) p % HEAP_OBJECT_ALIGN == 0);
	ASSERT((uintptr_t) size % HEAP_OBJECT_ALIGN == 0);
	ASSERT((uintptr_t) size >= HEAP_OBJECT_ALIGN);
	FreeSpace *space = (FreeSpace *) p;
	space->next = NULL;
	space->size = size;
	space->tags = TAG_FREESPACE;
	return space;
}


static FreeSpace *createInitialFreeSpace(struct HeapPage *page)
{
	uint8_t *start = (uint8_t *) align((uintptr_t) page->body, HEAP_OBJECT_ALIGN);
	return createFreeSpace(start, align(page->bodySize - HEAP_OBJECT_ALIGN / 2, HEAP_OBJECT_ALIGN));
}


void freeListPrint(FreeList *freeList)
{
#if FREE_LIST_COLLECT_STATS
	printf(
		"Free list #%p (alloc exact: %zu next: %zu fallback: %zu average size: %zu B, added spaces: %zu average size: %zu B, expanded: %zu)\n",
		freeList,
		freeList->stats.exactAllocs,
		freeList->stats.nextAllocs,
		freeList->stats.fallbackAllocs,
		freeList->stats.averageSize,
		freeList->stats.addedFreeSpaces,
		freeList->stats.averageAddedSpaceSize,
		freeList->stats.expanded
	);
#else
    printf("Free list #%p\n", (void*)freeList);
#endif

	for (size_t i = 0; i <= FREE_LIST_SIZE; i++) {
		_Bool hasSpace = (freeList->freeMap[i / 8] & (1 << (i % 8))) != 0;
		if (hasSpace) {
			printf("%zu.%s\n", i, hasSpace ? " [has space]" : "");
			freeSpacePrint(freeList->freeSpaces[i]);
		}
	}
}


static void freeSpacePrint(FreeSpace *freeSpace)
{
    if (freeSpace == NULL) return;

    ASSERT((freeSpace->tags & TAG_FREESPACE) != 0);

    size_t count = 1;
    FreeSpace *next = freeSpace->next;
    while (next != NULL && next->size == freeSpace->size) {
        ASSERT((next->tags & TAG_FREESPACE) != 0);
        count++;
        next = next->next;
    }

    if (count > 1) {
        printf("\t%llu B * %zu\n", (unsigned long long)freeSpace->size, count);
    } else {
        printf("\t%llu B\n", (unsigned long long)freeSpace->size);
    }
    freeSpacePrint(next);
}
