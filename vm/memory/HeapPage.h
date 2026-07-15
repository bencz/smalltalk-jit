#ifndef PAGE_H
#define PAGE_H

#include "core/Object.h"
#include "memory/FreeList.h"
#include <stddef.h>
#include <stdint.h>

typedef struct HeapPage {
	struct HeapPage *next;
	_Bool isExecutable;
	size_t size;
	size_t bodySize;
	uint8_t *body;
} HeapPage;

// A page's [base,end) body range, kept in a sorted array for O(log P) membership
// tests (pageSpaceIncludes is otherwise an O(P) list walk on a hot GC path).
typedef struct {
	uint8_t *base;
	uint8_t *end;
} PageRange;

typedef struct {
	HeapPage *pages;
	HeapPage *pagesTail;
	FreeList freeList;
	PageRange *ranges;   // sorted by base; parallel index over `pages`
	size_t rangeCount;
	size_t rangeCap;
	size_t totalBytes;   // sum of mapped page bodySizes (space capacity)
} PageSpace;

typedef struct {
	HeapPage *page;
	FreeSpace *current;
} PageSpaceIterator;

void initPageSpace(PageSpace *pageSpace, size_t size, _Bool executable);
void freePageSpace(PageSpace *pageSpace);
HeapPage *mapHeapPage(size_t size, _Bool executable);
void unmapHeapPage(HeapPage *page);
// Keep the sorted range index in sync with the page list. Call after linking a
// new page / before unlinking a page from `pageSpace->pages`.
void pageSpaceIndexAdd(PageSpace *pageSpace, HeapPage *page);
void pageSpaceIndexRemove(PageSpace *pageSpace, HeapPage *page);
_Bool heapPageIncludes(HeapPage *page, uint8_t *addr);
uint8_t *pageSpaceTryAllocate(PageSpace *pageSpace, size_t size);
HeapPage *pageSpaceFindPage(PageSpace *PageSpace, uint8_t *addr);
_Bool pageSpaceIncludes(PageSpace *PageSpace, uint8_t *addr);
void pageSpaceIteratorInit(PageSpaceIterator *iterator, PageSpace *space);
RawObject *pageSpaceIteratorNext(PageSpaceIterator *iterator);


static uintptr_t align(uintptr_t v, size_t align)
{
	return (v + (align - 1)) & -align;
}

#endif
