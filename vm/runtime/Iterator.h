#ifndef ITERATOR_H
#define ITERATOR_H

#include "core/Object.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"

// GC-SAFE by construction: holds a HANDLE to the backing Array plus indices,
// never raw interior pointers. The old raw start/end/current form dangled the
// moment a loop body allocated enough to scavenge (the collection moves, the
// iterator keeps reading the abandoned semispace); that stayed latent for as
// long as from-space survived one scavenge intact, and blew up as intermittent
// corruption once two scavenges landed inside one iteration window
// (root-caused via from-space poisoning on ActorStressTest).
typedef struct {
	Array *array;      // handle to the backing Array
	ptrdiff_t base;    // index of the first element (for iteratorIndex)
	ptrdiff_t index;
	ptrdiff_t limit;
} Iterator;

void initArrayIterator(Iterator *iterator, Array *array, ptrdiff_t from, ptrdiff_t to);
void initOrdCollIterator(Iterator *iterator, OrderedCollection *ordColl, ptrdiff_t from, ptrdiff_t to);
void initDictIterator(Iterator *iterator, Dictionary *dict);
ptrdiff_t iteratorIndex(Iterator *iterator);
_Bool iteratorHasNext(Iterator *iterator);
Value iteratorNext(Iterator *iterator);
Object *iteratorNextObject(Iterator *iterator);

#endif
