#include "core/ClassTable.h"
#include "core/Assert.h"
#include <stdio.h>
#include <stdlib.h>

#define CLASS_TABLE_INITIAL 512


void classTableInit(ClassTable *table)
{
	table->capacity = CLASS_TABLE_INITIAL;
	table->entries = calloc(table->capacity, sizeof(RawObject *));
	ASSERT(table->entries != NULL);
	table->size = CLASS_INDEX_FIRST;
	table->freeHead = 0;
}


void classTableFree(ClassTable *table)
{
	free(table->entries);
	table->entries = NULL;
	table->size = 0;
	table->capacity = 0;
	table->freeHead = 0;
}


static void classTableGrow(ClassTable *table, size_t needed)
{
	if (needed < table->capacity) {
		return;
	}
	size_t capacity = table->capacity;
	while (capacity <= needed) {
		capacity *= 2;
	}
	// 22 bits is a real ceiling, so it gets a real error. A wrap here would
	// silently give two classes the same identity, which every inline-cache
	// guard in the system would then believe.
	if (capacity > CLASS_INDEX_MAX + 1) {
		capacity = CLASS_INDEX_MAX + 1;
	}
	if (needed >= capacity) {
		fprintf(stderr, "class table exhausted: %zu classes is the 22-bit ceiling\n",
			(size_t) CLASS_INDEX_MAX);
		abort();
	}
	RawObject **entries = realloc(table->entries, capacity * sizeof(RawObject *));
	ASSERT(entries != NULL);
	for (size_t i = table->capacity; i < capacity; i++) {
		entries[i] = NULL;
	}
	table->entries = entries;
	table->capacity = capacity;
}


uint32_t classTableAdd(ClassTable *table, RawObject *class)
{
	// A freed slot is reused before the table grows: class indices are handed
	// out for the life of the process and a long-running image that defines and
	// forgets classes would otherwise walk the 22-bit space.
	if (table->freeHead != 0) {
		size_t index = table->freeHead;
		table->freeHead = CLASS_TABLE_LINK_INDEX(table->entries[index]);
		table->entries[index] = class;
		return (uint32_t) index;
	}
	classTableGrow(table, table->size);
	uint32_t index = (uint32_t) table->size++;
	table->entries[index] = class;
	return index;
}


void classTableSet(ClassTable *table, uint32_t index, RawObject *class)
{
	ASSERT(index != CLASS_INDEX_INVALID);
	classTableGrow(table, index);
	table->entries[index] = class;
	if (index >= table->size) {
		table->size = index + 1;
	}
}


void classTableRemove(ClassTable *table, uint32_t index)
{
	ASSERT(index != CLASS_INDEX_INVALID && index < table->size);
	// The free chain threads through the entry itself, stored ODD so the
	// collector's walk can never mistake it for a (16-aligned) heap pointer.
	table->entries[index] = CLASS_TABLE_FREE_LINK(table->freeHead);
	table->freeHead = index;
}
