#include "memory/RememberedSet.h"
#include "core/Assert.h"


void rememberedSetGrow(RememberedSet *set)
{
	RememberedSetBlock *block = malloc(sizeof(RememberedSetBlock));
	ASSERT(block != NULL);
	block->prev = set->blocks;
	block->current = block->objects;
	set->blocks = block;
}


void rememberedSetFreeBlocks(RememberedSetBlock *block)
{
	while (block != NULL) {
		RememberedSetBlock *prev = block->prev;
		free(block);
		block = prev;
	}
}


size_t rememberedSetCount(RememberedSet *set)
{
	size_t count = 0;
	for (RememberedSetBlock *block = set->blocks; block != NULL; block = block->prev) {
		count += (size_t) (block->current - block->objects);
	}
	return count;
}
