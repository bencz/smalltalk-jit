#ifndef CLASS_TABLE_H
#define CLASS_TABLE_H

// The class table: the indirection the 22-bit classIndex in the object header
// buys (ADR 0005).
//
// It exists so that class IDENTITY is a small integer that never changes, while
// the class OBJECT stays an ordinary movable heap object. Three consequences,
// and they are the reason the header changed at all:
//
//   * an inline-cache guard is `cmp dword [obj], imm32`, with no load of the
//     class and no second cache line touched;
//   * a class baked into generated code is an immediate that no collection ever
//     has to relocate, which retires most of the old baked-pointer machinery;
//   * inline-cache cells stop being wiped at every collection, because what they
//     hold is an index and not an address that dies with the GC epoch. That is
//     what makes the type profile of phase 2 cumulative instead of amnesiac.
//
// The table itself IS a root set: every live class is reachable through it, and
// the collector updates each entry when the class object moves. Nothing else in
// the VM may hold a bare RawClass* across a safepoint.

#include "core/Object.h"
#include <stddef.h>

typedef struct {
	RawObject **entries; // index -> class object, NULL for a free slot
	size_t size;         // highest index handed out, plus one
	size_t capacity;
	size_t freeHead;     // head of the free-slot chain, or 0 when empty
} ClassTable;

// Indices 0..CLASS_INDEX_FIRST-1 are reserved so a zeroed word is never a valid
// class: a garbage header then fails the very first check instead of naming a
// real class. Index 0 is permanently invalid for that reason.
#define CLASS_INDEX_INVALID 0
#define CLASS_INDEX_FIRST 1

void classTableInit(ClassTable *table);
void classTableFree(ClassTable *table);
// Hand out a fresh index for `class`, or abort cleanly if 22 bits are exhausted.
uint32_t classTableAdd(ClassTable *table, RawObject *class);
// Bind a specific index (snapshot load, which restores the exact numbering).
void classTableSet(ClassTable *table, uint32_t index, RawObject *class);
void classTableRemove(ClassTable *table, uint32_t index);


// A free slot threads the next free index through the entry itself. It is
// stored ODD so it can never be mistaken for a heap pointer, which is always
// 16-aligned: the collector walks this table and must be able to tell a class
// from a free-list link with no side table and no ambiguity.
#define CLASS_TABLE_FREE_LINK(index) ((RawObject *) (((uintptr_t) (index) << 1) | 1))
#define CLASS_TABLE_LINK_INDEX(entry) ((size_t) ((uintptr_t) (entry) >> 1))


static inline _Bool classTableIsLive(RawObject *entry)
{
	return entry != NULL && ((uintptr_t) entry & 1) == 0;
}


static inline RawObject *classTableAt(ClassTable *table, uint32_t index)
{
	ASSERT(index != CLASS_INDEX_INVALID && index < table->size);
	ASSERT(classTableIsLive(table->entries[index]));
	return table->entries[index];
}

#endif
