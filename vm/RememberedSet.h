#ifndef REMEMBERED_SET
#define REMEMBERED_SET

#define REMEMBERED_SET_BLOCK_SIZE 1024

typedef struct RememberedSetBlock {
	struct RememberedSetBlock *prev;
	Value *current;
	Value *end;
	Value objects[REMEMBERED_SET_BLOCK_SIZE];
} RememberedSetBlock;

typedef struct {
	RememberedSetBlock *blocks;
} RememberedSet;

typedef struct {
	RememberedSetBlock *block;
	Value *blockEnd;
	RememberedSetBlock *firstBlock;
	_Bool hasPrevBlock;
	Value *next;
} RememberedSetIterator;

static void rememberedSetGrow(RememberedSet *rememberedSet);
static RememberedSetBlock *createRememberedSetBlock(RememberedSetBlock *prev);
static RememberedSetBlock *rememberedSetBlockFirst(RememberedSetBlock *block);
static void rememberedSetIteratorSetBlock(RememberedSetIterator *iterator, RememberedSetBlock *block);


static void initRememberedSet(RememberedSet *rememberedSet)
{
	rememberedSet->blocks = createRememberedSetBlock(NULL);
}


static void rememberedSetAdd(RememberedSet *rememberedSet, RawObject *object)
{
	// Grow BEFORE writing when the head block is full, so the store is always in
	// bounds. (A grow-AFTER-write scheme corrupts the C heap the moment the head
	// is ever observed full on entry, because the store has already run.) Every
	// remembered-set writer — the C barrier, the scavenger, and the JIT barrier
	// (generateStoreCheck) — funnels through here so the invariant holds.
	RememberedSetBlock *block = rememberedSet->blocks;
	if (block->current >= block->end) {
		rememberedSetGrow(rememberedSet);
		block = rememberedSet->blocks;
	}
	ASSERT((object->tags & TAG_REMEMBERED) == 0);
	object->tags |= TAG_REMEMBERED;
	*block->current++ = tagPtr(object);
}


static void rememberedSetGrow(RememberedSet *rememberedSet)
{
	rememberedSet->blocks = createRememberedSetBlock(rememberedSet->blocks);
}


static RememberedSetBlock *createRememberedSetBlock(RememberedSetBlock *prev)
{
	RememberedSetBlock *block = malloc(sizeof(RememberedSetBlock));
	ASSERT(block != NULL);
	block->prev = prev;
	block->current = block->objects;
	block->end = block->current + REMEMBERED_SET_BLOCK_SIZE;
	return block;
}


static void rememberedSetReset(RememberedSet *rememberedSet)
{
	RememberedSetBlock *block = rememberedSet->blocks;
	RememberedSetBlock *prev = block->prev;
	while (prev != NULL) {
		RememberedSetBlock *tmp = prev;
		prev = tmp->prev;
		free(tmp);
	}

	block->prev = NULL;
	block->current = block->objects;
}


static void initRememberedSetIterator(RememberedSetIterator *iterator, RememberedSet *rememberedSet)
{
	RememberedSetBlock *block = rememberedSet->blocks;
	iterator->firstBlock = rememberedSetBlockFirst(block);
	rememberedSetIteratorSetBlock(iterator, block);
	if (iterator->next == NULL && block != iterator->firstBlock) {
		rememberedSetIteratorSetBlock(iterator, block->prev);
	}
}


static RememberedSetBlock *rememberedSetBlockFirst(RememberedSetBlock *block)
{
	while (block->prev != NULL) {
		block = block->prev;
	}
	return block;
}


static RawObject *rememberedSetIteratorNext(RememberedSetIterator *iterator)
{
	Value *next = iterator->next;
	ASSERT(next != NULL);
	RawObject *object = asObject(*next++);
	if (next < iterator->blockEnd) {
		iterator->next = next;
	} else if (iterator->hasPrevBlock) {
		rememberedSetIteratorSetBlock(iterator, iterator->block->prev);
	} else {
		iterator->next = NULL;
	}
	return object;
}


static void rememberedSetIteratorSetBlock(RememberedSetIterator *iterator, RememberedSetBlock *block)
{
	ASSERT(block != NULL);
	iterator->block = block;
	iterator->blockEnd = block->current;
	iterator->hasPrevBlock = block != iterator->firstBlock;
	iterator->next = block->objects < iterator->blockEnd ? block->objects : NULL;
}


static _Bool rememberedSetIteratorHasNext(RememberedSetIterator *iterator)
{
	return iterator->next != NULL;
}

#endif
