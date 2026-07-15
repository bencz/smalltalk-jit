#include "memory/HeapPage.h"
#include "core/CompiledCode.h"
#include "core/Assert.h"
#include "jit/TargetTraits.h" // TARGET_CODE_FILLER_BYTE
#include "os/Os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRINT_PAGE_ALLOC 0


void initPageSpace(PageSpace *pageSpace, size_t size, _Bool executable)
{
	pageSpace->ranges = NULL;
	pageSpace->rangeCount = 0;
	pageSpace->rangeCap = 0;
	pageSpace->totalBytes = 0;
	HeapPage *page = mapHeapPage(size, executable);
	pageSpace->pages = pageSpace->pagesTail = page;
	initFreeList(&pageSpace->freeList, page);
	pageSpaceIndexAdd(pageSpace, page);
}


void freePageSpace(PageSpace *pageSpace)
{
	HeapPage *page = pageSpace->pages;
	while (page != NULL) {
		HeapPage *next = page->next;
		unmapHeapPage(page);
		page = next;
	}
	free(pageSpace->ranges);
	pageSpace->ranges = NULL;
	pageSpace->rangeCount = pageSpace->rangeCap = 0;
}


// ---- sorted [base,end) range index over the page list --------------------
// Ranges are kept sorted by base so membership is a binary search. Pages map at
// arbitrary (non-overlapping) mmap addresses, so add/remove are O(P) shifts, but
// those happen only on page map/unmap — vastly rarer than the membership tests
// on the GC hot path.

void pageSpaceIndexAdd(PageSpace *pageSpace, HeapPage *page)
{
	if (pageSpace->rangeCount == pageSpace->rangeCap) {
		pageSpace->rangeCap = pageSpace->rangeCap ? pageSpace->rangeCap * 2 : 16;
		pageSpace->ranges = realloc(pageSpace->ranges, pageSpace->rangeCap * sizeof(PageRange));
	}
	uint8_t *base = page->body;
	size_t lo = 0, hi = pageSpace->rangeCount;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (pageSpace->ranges[mid].base < base) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	memmove(&pageSpace->ranges[lo + 1], &pageSpace->ranges[lo],
		(pageSpace->rangeCount - lo) * sizeof(PageRange));
	pageSpace->ranges[lo].base = base;
	pageSpace->ranges[lo].end = page->body + page->bodySize;
	pageSpace->rangeCount++;
	pageSpace->totalBytes += page->bodySize;
}


void pageSpaceIndexRemove(PageSpace *pageSpace, HeapPage *page)
{
	uint8_t *base = page->body;
	size_t lo = 0, hi = pageSpace->rangeCount;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (pageSpace->ranges[mid].base < base) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo < pageSpace->rangeCount && pageSpace->ranges[lo].base == base) {
		memmove(&pageSpace->ranges[lo], &pageSpace->ranges[lo + 1],
			(pageSpace->rangeCount - lo - 1) * sizeof(PageRange));
		pageSpace->rangeCount--;
		pageSpace->totalBytes -= page->bodySize;
	}
}


HeapPage *mapHeapPage(size_t size, _Bool executable)
{
	size_t alignedSize = align(size, osPageSize());
	HeapPage *page = osPageAlloc(alignedSize, executable);

	if (page == NULL) {
		FAIL();
	}

	page->next = NULL;
	page->isExecutable = executable;
	page->size = alignedSize;
	page->bodySize = alignedSize - sizeof(*page);
	page->body = (uint8_t *) page + sizeof(*page);
	// PORT_ME(wxorx): executable pages are mapped RWX and never remapped; a
	// hardened kernel or a W^X-enforcing arch needs a write->exec protocol here.
	memset(page->body, executable ? TARGET_CODE_FILLER_BYTE : 0, page->bodySize);
	page->bodySize -= page->bodySize % HEAP_OBJECT_ALIGN;
#if PRINT_PAGE_ALLOC
	printf("Page %p %zu%s\n", page, size, executable ? " executable" : "");
#endif
	return page;
}


void unmapHeapPage(HeapPage *page)
{
	if (!osPageFree(page, page->size)) {
		FAIL();
	}
}


_Bool heapPageIncludes(HeapPage *page, uint8_t *addr)
{
	return page->body <= addr && addr < page->body + page->bodySize;
}


uint8_t *pageSpaceTryAllocate(PageSpace *pageSpace, size_t size)
{
	ASSERT(size % HEAP_OBJECT_ALIGN == 0);
	uint8_t *p = freeListTryAllocate(&pageSpace->freeList, size);
	ASSERT(p == NULL || pageSpaceIncludes(pageSpace, p));
	return p;
}


HeapPage *pageSpaceFindPage(PageSpace *pageSpace, uint8_t *addr)
{
	HeapPage *page = pageSpace->pages;

	while (page != NULL) {
		if (page->body <= addr && addr < page->body + page->bodySize) {
			return page;
		}
		page = page->next;
	}
	return NULL;
}


_Bool pageSpaceIncludes(PageSpace *pageSpace, uint8_t *addr)
{
	// Rightmost range whose base <= addr, then check addr is before its end.
	// Reads only VM-owned page metadata (never dereferences `addr`), so it is
	// safe on a possibly-garbage class pointer (plausibleObject's guarantee).
	size_t lo = 0, hi = pageSpace->rangeCount;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (pageSpace->ranges[mid].base <= addr) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo == 0) {
		return 0;
	}
	return addr < pageSpace->ranges[lo - 1].end;
}


void pageSpaceIteratorInit(PageSpaceIterator *iterator, PageSpace *space)
{
	iterator->page = space->pages;
	iterator->current = (FreeSpace *) align((uintptr_t) iterator->page->body, HEAP_OBJECT_ALIGN);
}


RawObject *pageSpaceIteratorNext(PageSpaceIterator *iterator)
{
	FreeSpace *object = iterator->current;
	if ((uint8_t *) object >= iterator->page->body + iterator->page->bodySize) {
		if (iterator->page->next == NULL) {
			return NULL;
		}
		iterator->page = iterator->page->next;
		object = (FreeSpace *) align((uintptr_t) iterator->page->body, HEAP_OBJECT_ALIGN);
	}

	size_t size;
	if (object->tags & TAG_FREESPACE) {
		size = object->size;
	} else if (iterator->page->isExecutable) {
		size = computeNativeCodeSize((NativeCode *) object);
	} else {
		size = computeRawObjectSize((RawObject *) object);
	}
	iterator->current = (FreeSpace *) ((uint8_t *) object + align(size, HEAP_OBJECT_ALIGN));
	return (RawObject *) object;
}
