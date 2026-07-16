#include "runtime/Iterator.h"
#include "core/Handle.h"
#include "core/Assert.h"


void initArrayIterator(Iterator *iterator, Array *array, ptrdiff_t from, ptrdiff_t to)
{
	ASSERT(array->raw->class == Handles.Array->raw);
	iterator->array = array;
	iterator->base = from;
	iterator->index = from;
	iterator->limit = array->raw->size + to;
}


void initOrdCollIterator(Iterator *iterator, OrderedCollection *ordColl, ptrdiff_t from, ptrdiff_t to)
{
	ASSERT(ordColl->raw->class == Handles.OrderedCollection->raw);
	// snapshot the CURRENT contents array through a handle (a mid-iteration
	// grow swaps the collection's array; like the old form, we keep iterating
	// the original one, but now GC-safely)
	iterator->array = (Array *) scopeHandle((RawObject *) ordCollGetContents(ordColl));
	iterator->base = ordCollGetFirstIndex(ordColl) + from - 1;
	iterator->index = iterator->base;
	iterator->limit = iterator->base + ordCollSize(ordColl) + to;
}


void initDictIterator(Iterator *iterator, Dictionary *dict)
{
	ASSERT(dict->raw->class == Handles.Dictionary->raw);
	initArrayIterator(iterator, dictGetContents(dict), 0, 0);
}


ptrdiff_t iteratorIndex(Iterator *iterator)
{
	return iterator->index - iterator->base;
}


_Bool iteratorHasNext(Iterator *iterator)
{
	return iterator->index < iterator->limit;
}


Value iteratorNext(Iterator *iterator)
{
	// re-read through the handle EVERY step: the loop body may allocate, a
	// scavenge may move the array, and the handle is what stays current
	return iterator->array->raw->vars[iterator->index++];
}


Object *iteratorNextObject(Iterator *iterator)
{
	return scopeHandle(asObject(iteratorNext(iterator)));
}
