// Gate level 1: the heap allocates and collects, with no execution engine.
//
// This runs against the memory subsystem ALONE. It links no compiler, no
// runtime and no JIT, which is the point: after the dry cut (ADR 0002) the
// collector is the first thing that can be proved correct on its own, and
// proving it before anything is built on top is the only cheap moment.
//
// What it checks, and why each one:
//
//   1. an object survives a young collection and keeps its contents
//   2. an UNREACHABLE object does not, and its space comes back
//   3. a pointer is UPDATED in place when its target moves
//   4. two survivals promote to the old space (the aging policy)
//   5. an old-to-young edge recorded by the write barrier keeps the young
//      object alive, which is the invariant the whole generational scheme
//      rests on and the one whose absence produces silent corruption
//   6. FORMAT_DOUBLES and FORMAT_BYTES bodies are NEVER scanned: a double
//      whose bit pattern looks exactly like a heap pointer must be left alone.
//      This is requirement R5, and it is what makes phase 7's flat value
//      arrays free to collect
//   7. a full collection reclaims old-space garbage and the heap still walks

#include "memory/Collector.h"
#include "memory/Heap.h"
#include "memory/ObjectWalk.h"
#include "core/ClassTable.h"
#include "core/Handle.h"
#include "core/Thread.h"
#include <stdio.h>
#include <string.h>

__thread Thread CurrentThread;
ptrdiff_t gCurrentThreadTpoff;

static int gFailures;
static int gChecks;

static void check(const char *what, int ok)
{
	gChecks++;
	if (!ok) {
		gFailures++;
		printf("  FAIL  %s\n", what);
	} else {
		printf("  ok    %s\n", what);
	}
}


// A class is an ordinary MOVABLE object, so this test holds classes by INDEX and
// never by pointer. That is not a detail of the test, it is the point of ADR
// 0005: the index is the stable identity and the address is not. Written the
// other way round first, this file crashed the moment the collector started
// promoting classes correctly, with a shape read out of a class that had moved.
static uint32_t makeClass(Heap *heap, ObjectFormat format, uint16_t fixedSlots,
	uint8_t rawWords, uint8_t pointerWords)
{
	size_t bytes = objectAlignSize(sizeof(RawClass));
	uint8_t *address = allocate(heap, bytes);
	RawClass *class = (RawClass *) address;
	// A class is itself an object: a header, tagged fields, then a raw
	// trailer (its shape and its own index). FORMAT_MIXED_BYTES describes
	// exactly that, with the trailer counted as the trailing raw region.
	uint32_t index = classTableAdd(&heap->classes, (RawObject *) class);
	class->header = makeObjectHeader(index, 1, FORMAT_MIXED_BYTES,
		bytes / sizeof(uint64_t));
	class->instanceShape.format = (uint8_t) format;
	class->instanceShape.fixedSlots = fixedSlots;
	class->instanceShape.rawWords = rawWords;
	class->instanceShape.pointerWords = pointerWords;
	class->classIndex = index;
	// A class object describes ITSELF as raw words followed by its nine tagged
	// fields; the shape above is what it stamps onto its INSTANCES.
	class->instanceShape.rawWords = rawWords;
	class->instanceShape.pointerWords = pointerWords;
	return index;
}


// The current address of a class. Re-read every time, never cached.
static RawObject *classAt(Heap *heap, uint32_t index)
{
	return classTableAt(&heap->classes, index);
}


int main(void)
{
	Heap heap;
	CurrentThread.tlab.top = NULL;
	CurrentThread.tlab.end = NULL;
	initRememberedSet(&CurrentThread.rememberedSet);
	initHeap(&heap, &CurrentThread);
	CurrentThread.heap = &heap;
	heap.mutators = &CurrentThread;
	CurrentThread.nextMutator = NULL;

	printf("gate level 1: heap allocates and collects, no execution engine\n\n");

	// Classes are FORMAT_MIXED_BYTES over their own tagged fields. They are held
	// by INDEX below, because they move.
	uint32_t pairClass = makeClass(&heap, FORMAT_POINTERS, 2, 0, 9);
	uint32_t doubleClass = makeClass(&heap, FORMAT_DOUBLES, 0, 0, 9);
	uint32_t byteClass = makeClass(&heap, FORMAT_BYTES, 0, 0, 9);
#define classOfPairs classAt(&heap, pairClass)
#define classOfDoubles classAt(&heap, doubleClass)
#define classOfBytes classAt(&heap, byteClass)

	// ---- 1, 2, 3: survival, reclamation, in-place update -------------------
	Value roots[4] = { 0, 0, 0, 0 };
	collectorSetExtraRoots(roots, 4);

	RawObject *live = allocateObject(&heap, classOfPairs, 0);
	((Value *) live->body)[0] = tagInt(1234);
	((Value *) live->body)[1] = tagInt(0);
	roots[0] = tagPtr(live);

	// Unreachable from any root: nothing points at it after this statement.
	RawObject *dead = allocateObject(&heap, classOfPairs, 0);
	((Value *) dead->body)[0] = tagInt(9999);
	RawObject *deadWas = dead;

	// A chain, so the Cheney scan has something transitive to do.
	RawObject *tail = allocateObject(&heap, classOfPairs, 0);
	((Value *) tail->body)[0] = tagInt(4321);
	((Value *) live->body)[1] = tagPtr(tail);

	RawObject *liveWas = live;
	collectorScavenge(&heap);

	RawObject *liveNow = asObject(roots[0]);
	check("reachable object survives a young collection", liveNow != NULL);
	check("survivor keeps its contents",
		((Value *) liveNow->body)[0] == tagInt(1234));
	check("root slot is updated in place when the target moves",
		liveNow != liveWas);
	RawObject *tailNow = asObject(((Value *) liveNow->body)[1]);
	check("transitively reachable object survives",
		((Value *) tailNow->body)[0] == tagInt(4321));
	check("unreachable object is left behind",
		!nurseryInFromSpace(&heap.newSpace, (uint8_t *) deadWas)
		|| rawObjectIsForwarded(deadWas) == 0);

	// ---- 4: two survivals promote ------------------------------------------
	check("first survival stays young", isNewObject(liveNow));
	collectorScavenge(&heap);
	liveNow = asObject(roots[0]);
	check("second survival promotes to the old space", isOldObject(liveNow));

	// And a PROMOTED object's slots are scanned like any other survivor's.
	//
	// Cheney's scan pointer walks to-space in address order, and that is the
	// complete work list only for objects copied INTO to-space. One promoted to
	// the old space is just as much a survivor with just as many stale slots,
	// and it lies nowhere in that range. Omitting it is invisible at collection
	// time -- the object is intact, the collection reports success -- and
	// surfaces arbitrarily later as a slot still naming a corpse in the dead
	// semispace. It is checked here, at the collection that promotes, because
	// this is the only place the cost of finding it is small.
	// The GENERATION check is the load-bearing one, and that is worth knowing:
	// with the promotion scan removed, the contents check below still passes,
	// because nothing ever forwarded the abandoned object and its body still
	// literally reads 4321. The stale pointer only turns into wrong data once
	// the semispace is reused, which is an arbitrary distance away from the bug.
	// So the check has to be "is this pointer where a survivor can be", not "does
	// what it points at still look right".
	RawObject *tailAfter = asObject(((Value *) liveNow->body)[1]);
	check("its target was promoted alongside it, so the slot names the SURVIVOR "
		"and not the corpse in the evacuated semispace", isOldObject(tailAfter));
	check("and that survivor kept its contents",
		((Value *) tailAfter->body)[0] == tagInt(4321));

	// ---- an INDEXED object that ALSO has a named slot ----------------------
	//
	// A Closure is one: element count, then the method, then the captures. Its
	// size cannot be derived from the element count, because the count says
	// nothing about the named slot, and deriving it that way answered ONE WORD
	// SHORT: the walk strode into the middle of the object and the pointer range
	// it computed stopped before the named slot, so nothing ever updated it.
	//
	// The worst case is zero elements, which is a block that captures nothing,
	// and it is the case used here.
	//
	// THE GENERATION IS THE CHECK, and the contents are not. The target left
	// behind in the evacuated semispace still reads 4242, so "is the field
	// intact" answers YES with the bug present. Only a collector that FOUND it
	// twice could have promoted it.
	uint32_t namedIndexed = makeClass(&heap, FORMAT_INDEXED_POINTERS, 1, 0, 9);
	{
		RawObject *withNamed = allocateObject(&heap, classAt(&heap, namedIndexed), 0);
		RawObject *namedTarget = allocateObject(&heap, classOfPairs, 0);
		((Value *) namedTarget->body)[0] = tagInt(4242);
		// Body word 0 is the element count; word 1 is the named slot.
		((Value *) withNamed->body)[1] = tagPtr(namedTarget);
		roots[3] = tagPtr(withNamed);

		collectorScavenge(&heap);
		collectorScavenge(&heap);

		Value slot = ((Value *) asObject(roots[3])->body)[1];
		check("the named slot of an indexed object is scanned, so its target was "
			"promoted rather than abandoned in the dead semispace",
			valueTypeOf(slot, VALUE_POINTER) && isOldObject(asObject(slot)));
		check("and that target kept its contents",
			valueTypeOf(slot, VALUE_POINTER)
			&& ((Value *) asObject(slot)->body)[0] == tagInt(4242));
		roots[3] = 0;
	}

	// ---- a fresh object lands on RECYCLED memory ---------------------------
	//
	// The semispaces are never re-zeroed when they flip, so from the second
	// cycle onward an allocation lands on the bytes of a dead object. Every word
	// the collector will scan therefore has to be written by the ALLOCATOR: one
	// left alone holds whatever the corpse held, and a slot that used to be a
	// pointer is followed by the next collection.
	//
	// The padding is the part that is easy to miss. A two-slot object occupies
	// three body words after alignment, and the pointer range comes from the
	// SIZE, so that third word is scanned and no caller ever writes it.
	{
		// Dirty both semispaces first, with values that are plausible pointers.
		for (int round = 0; round < 2; round++) {
			for (int i = 0; i < 20000; i++) {
				RawObject *garbage = allocateObject(&heap, classOfPairs, 0);
				((Value *) garbage->body)[0] = tagPtr(liveNow);
				((Value *) garbage->body)[1] = tagPtr(liveNow);
				((Value *) garbage->body)[2] = tagPtr(liveNow); // the padding word
			}
			collectorScavenge(&heap);
		}
		RawObject *fresh = allocateObject(&heap, classOfPairs, 0);
		size_t count;
		Value *slots = objectPointerSlots(&heap.classes, fresh, &count);
		_Bool clean = count == 3;
		for (size_t i = 0; i < count; i++) {
			clean = clean && slots[i] == 0; // nil does not exist yet in this test
		}
		check("every scanned word of a fresh object is written by the allocator, "
			"padding included, so recycled memory carries nothing forward", clean);
	}

	// ---- 6: raw bodies are never scanned -----------------------------------
	// A double whose bits are exactly a plausible tagged pointer. If the
	// collector ever scanned a FORMAT_DOUBLES body it would follow this and
	// either crash or corrupt; requirement R5 says it must not look.
	RawObject *doubles = allocateObject(&heap, classOfDoubles, 4);
	double *values = rawObjectDoubles(doubles);
	uint64_t poison = (uint64_t) (uintptr_t) liveNow | VALUE_POINTER;
	memcpy(&values[0], &poison, sizeof(poison));
	values[1] = 2.5;
	values[2] = -1.0;
	values[3] = 1e300;
	roots[1] = tagPtr(doubles);

	RawObject *bytes = allocateObject(&heap, classOfBytes, 24);
	memset(rawObjectBytes(bytes), 0xAB, 24);
	roots[2] = tagPtr(bytes);

	collectorScavenge(&heap);
	RawObject *doublesNow = asObject(roots[1]);
	RawObject *bytesNow = asObject(roots[2]);
	uint64_t readBack;
	memcpy(&readBack, &rawObjectDoubles(doublesNow)[0], sizeof(readBack));
	check("a double that looks like a pointer is left untouched",
		readBack == poison);
	check("the rest of a doubles body survives intact",
		rawObjectDoubles(doublesNow)[1] == 2.5
		&& rawObjectDoubles(doublesNow)[3] == 1e300);
	check("element count survives a copy",
		rawObjectElementCount(doublesNow) == 4);
	check("a bytes body survives intact",
		rawObjectBytes(bytesNow)[0] == 0xAB && rawObjectBytes(bytesNow)[23] == 0xAB);

	// ---- 5: the write barrier keeps a young object alive from an old one ---
	// `liveNow` is old by now. Point it at a fresh young object through the
	// barrier, drop every other reference, and collect: the young object must
	// survive purely because the barrier logged the edge.
	RawObject *youngChild = allocateObject(&heap, classOfPairs, 0);
	((Value *) youngChild->body)[0] = tagInt(777);
	check("the object holding the edge really is old", isOldObject(liveNow));
	rawObjectStorePtr(liveNow, &((Value *) liveNow->body)[1],
		youngChild);
	check("the write barrier logged the old-to-young edge",
		rawObjectHasGcBit(liveNow, GC_REMEMBERED));

	collectorScavenge(&heap);
	RawObject *childNow = asObject(((Value *) liveNow->body)[1]);
	check("young object survives ONLY via the remembered set",
		((Value *) childNow->body)[0] == tagInt(777));

	// ---- 7: a full collection reclaims and the heap still walks ------------
	size_t before = heap.oldSpace.allocated;
	for (int i = 0; i < 2000; i++) {
		RawObject *garbage = allocateObject(&heap, classOfPairs, 0);
		((Value *) garbage->body)[0] = tagInt(i);
	}
	collectorMarkSweep(&heap);
	check("a full collection keeps the reachable set",
		((Value *) asObject(roots[0])->body)[0] == tagInt(1234));
	check("a full collection reclaims old-space garbage",
		heap.oldSpace.allocated <= before + 64 * 1024);

	size_t walked = 0;
	PageSpaceIterator iterator;
	pageSpaceIteratorInit(&iterator, &heap.oldSpace, &heap.classes);
	for (RawObject *object = pageSpaceIteratorNext(&iterator);
			object != NULL; object = pageSpaceIteratorNext(&iterator)) {
		walked++;
	}
	check("the old space still walks end to end after a sweep", walked > 0);

	// ---- 7b: a collection TRIGGERED BY an old-space allocation ---------------
	//
	// The one above calls collectorMarkSweep directly, which is the easy case:
	// nothing is half-allocated at that moment. The hard case is the collection
	// the ALLOCATOR itself starts on crossing its threshold, and the order there
	// is the whole thing: carve first and the sweep walks a region that has been
	// handed out but whose header the caller has not written yet, so the walk
	// reads zeroes and strides by zero.
	//
	// The stride assertion is the LUCKY half. The other half is that the sweep
	// frees what is not marked, and nothing marks an object that does not exist
	// yet, so the fresh allocation lands on a free list while its caller is still
	// about to initialize it: one address, two owners, discovered somewhere else
	// entirely.
	//
	// The check is that the walk stays consistent, which is what a stride of zero
	// destroys. Verified by putting the collection back after the carve: this
	// aborts inside pageSpaceSweep.
	{
		// BIG objects, because only those reach allocateOld: anything that fits
		// the header's size field is a nursery allocation and never crosses this
		// threshold at all. That is why the first version of this check passed
		// with the bug in place.
		size_t elements = SIZE_INLINE_MAX_BYTES + 64;
		size_t fullsBefore = LastGCStats.count;
		heap.oldGcThreshold = heap.oldSpace.allocated + 256 * 1024;
		for (int i = 0; i < 400; i++) {
			RawObject *big = allocateObject(&heap, classOfBytes, elements);
			big->body[sizeof(uint64_t)] = (uint8_t) i;
		}
		check("allocating big objects triggered a full collection from INSIDE "
			"the allocator, which is the path this checks",
			LastGCStats.count > fullsBefore);

		size_t seen = 0;
		PageSpaceIterator each;
		pageSpaceIteratorInit(&each, &heap.oldSpace, &heap.classes);
		for (RawObject *object = pageSpaceIteratorNext(&each);
				object != NULL; object = pageSpaceIteratorNext(&each)) {
			seen++;
		}
		check("and the old space still walks end to end afterwards", seen > 0);
		check("and the reachable set came through it",
			((Value *) asObject(roots[0])->body)[0] == tagInt(1234));
	}

	// ---- 8: handles ---------------------------------------------------------
	// A handle is how C code holds an object across an allocation. The check
	// that matters is the second one: an object reachable ONLY through a handle
	// must survive, because that is the situation of every C function that
	// allocates while holding its operands.
	initHandles();
	{
		HandleScope scope;
		openHandleScope(&scope);

		RawObject *held = allocateObject(&heap, classOfPairs, 0);
		((Value *) held->body)[0] = tagInt(31337);
		Object *handle = scopeHandle(held);
		RawObject *heldWas = held;

		// Nothing else references it: roots[] is untouched here.
		collectorScavenge(&heap);
		check("an object reachable only through a handle survives",
			((Value *) handle->raw->body)[0] == tagInt(31337));
		check("the handle is updated when its object moves",
			handle->raw != heldWas);

		// Past the inline array and across TWO overflow chunks. Every handle
		// gets a distinct value and EVERY one is checked afterwards, because
		// the real hazard in the chunk walk is an off-by-one that visits the
		// wrong slice of the newest chunk: that would leave a handful of
		// handles unvisited, which shows up as a few wrong objects and not as
		// a crash. Checking only the ends would miss it entirely.
		enum { OVERFLOW_COUNT = HANDLE_SCOPE_INLINE + 2 * HANDLE_CHUNK_SIZE + 7 };
		static Object *many[OVERFLOW_COUNT];
		for (int i = 0; i < OVERFLOW_COUNT; i++) {
			RawObject *object = allocateObject(&heap, classOfPairs, 0);
			((Value *) object->body)[0] = tagInt(i);
			many[i] = scopeHandle(object);
		}
		collectorScavenge(&heap);
		int wrong = -1;
		for (int i = 0; i < OVERFLOW_COUNT; i++) {
			if (((Value *) many[i]->raw->body)[0] != tagInt(i)) {
				wrong = i;
				break;
			}
		}
		check("every handle across two overflow chunks is visited exactly once",
			wrong < 0);
		if (wrong >= 0) {
			printf("        first wrong handle: index %d of %d\n", wrong,
				(int) OVERFLOW_COUNT);
		}

		Object *promoted = closeHandleScope(&scope, handle);
		check("closing a scope with no parent promotes nothing", promoted == NULL);
	}
	check("closing the last scope leaves no open scope",
		CurrentThread.handleScopes == NULL);

	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
