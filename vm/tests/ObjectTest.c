// Gate level 2: the object layer. Strings, Symbols, Arrays, OrderedCollections.
//
// Still standalone, still no execution engine. What this proves that levels 0
// and 1 do not: that objects built by C code survive collection with their
// contents and their IDENTITY intact, which is the property everything above
// this line assumes without ever checking.
//
// The interesting checks are the identity ones. A Symbol is interned, so two
// Strings with the same characters must produce the SAME object, and it must
// still be the same object after a collection has moved it. Selector dispatch
// is a pointer compare, so if interning ever breaks, every send in the system
// silently starts missing.

#include "core/Class.h"
#include "core/Handle.h"
#include "memory/Collector.h"
#include "memory/Heap.h"
#include "memory/ObjectWalk.h"
#include "runtime/Collection.h"
#include "runtime/String.h"
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


// The bootstrap's first object cannot go through newObject, because newObject
// needs a class and this IS the class of classes. It is built by hand, once,
// and every class after it goes through the ordinary path.
static void bootstrapClassOfClasses(Heap *heap)
{
	size_t bytes = objectSizeForShape(CLASS_OF_CLASSES_SHAPE, CLASS_RAW_TRAILER_BYTES);
	RawClass *class = (RawClass *) allocate(heap, bytes);
	uint32_t index = classTableAdd(&heap->classes, (RawObject *) class);
	class->header = makeObjectHeader(index, index, FORMAT_MIXED_BYTES,
		bytes / sizeof(uint64_t));
	class->instanceShape = CLASS_OF_CLASSES_SHAPE;
	class->classIndex = index;
	Handles.ClassClass.raw = class;
}


static void bootstrapMinimal(Heap *heap)
{
	bootstrapClassOfClasses(heap);

	InstanceShape bytesShape = DEFINE_SHAPE(FORMAT_BYTES, 0, 0, 0);
	InstanceShape arrayShape = DEFINE_SHAPE(FORMAT_INDEXED_POINTERS, 0, 0, 0);
	InstanceShape ordCollShape = DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, 3);

	Handles.String.raw = classCreate(NULL, NULL, bytesShape)->raw;
	Handles.Symbol.raw = classCreate(&Handles.String, NULL, bytesShape)->raw;
	Handles.Array.raw = classCreate(NULL, NULL, arrayShape)->raw;
	Handles.OrderedCollection.raw = classCreate(NULL, NULL, ordCollShape)->raw;

	// The symbol table has to exist before the first intern, and it is a root
	// in its own right (part of SmalltalkHandles).
	Handles.symbolTable.raw = newArray(1024)->raw;
}


int main(void)
{
	Heap heap;
	CurrentThread.tlab.top = NULL;
	CurrentThread.tlab.end = NULL;
	CurrentThread.handleScopes = NULL;
	initRememberedSet(&CurrentThread.rememberedSet);
	initHeap(&heap, &CurrentThread);
	CurrentThread.heap = &heap;
	heap.mutators = &CurrentThread;
	CurrentThread.nextMutator = NULL;
	initHandles();

	printf("gate level 2: strings, symbols, arrays, ordered collections\n\n");

	HandleScope outer;
	openHandleScope(&outer);
	bootstrapMinimal(&heap);
	check("the class of classes describes itself",
		classShape(&Handles.ClassClass).format == FORMAT_MIXED_BYTES);
	check("classes get distinct table indices",
		classIndexOf(&Handles.String) != classIndexOf(&Handles.Symbol)
		&& classIndexOf(&Handles.Array) != classIndexOf(&Handles.String));

	// ---- strings -----------------------------------------------------------
	String *hello = stringFromC("hello");
	check("a string keeps its length", rawStringSize(hello->raw) == 5);
	check("a string keeps its bytes", stringEqualsC(hello, "hello"));
	check("a string is an instance of String",
		isInstanceOf((RawObject *) hello->raw, &Handles.String));

	String *hello2 = stringFromC("hello");
	check("two equal strings are equal but not identical",
		stringEquals(hello, hello2) && hello->raw != hello2->raw);
	check("hashing is by content, not identity",
		stringHash(hello->raw) == stringHash(hello2->raw));
	check("different content hashes differently",
		stringHash(hello->raw) != stringHash(stringFromC("world")->raw));

	// ---- symbols: identity is the whole point -----------------------------
	String *at = asSymbol(stringFromC("at:put:"));
	String *at2 = asSymbol(stringFromC("at:put:"));
	check("interning the same characters answers the SAME object",
		at->raw == at2->raw);
	check("a symbol is an instance of Symbol",
		isInstanceOf((RawObject *) at->raw, &Handles.Symbol));
	check("a symbol's identity hash is its content hash",
		rawObjectHash((RawObject *) at->raw) == stringHash(at->raw));
	check("selector arity comes from the selector's shape",
		selectorArgumentCount(at->raw) == 2
		&& selectorArgumentCount(stringFromC("size")->raw) == 0
		&& selectorArgumentCount(stringFromC("+")->raw) == 1);

	// Force the symbol table past its growth threshold, then check that
	// everything interned before is still interned to the same object.
	char buffer[32];
	for (int i = 0; i < 2000; i++) {
		snprintf(buffer, sizeof(buffer), "selector%d:", i);
		asSymbol(stringFromC(buffer));
	}
	String *atAgain = asSymbol(stringFromC("at:put:"));
	check("interning survives a symbol-table growth", atAgain->raw == at->raw);

	// ...and across a collection, which MOVES every symbol.
	RawObject *atWas = (RawObject *) at->raw;
	collectorScavenge(&heap);
	collectorScavenge(&heap);
	String *atAfterGc = asSymbol(stringFromC("at:put:"));
	check("interning survives a collection", atAfterGc->raw == at->raw);
	check("the symbol actually moved, so that was not a no-op",
		(RawObject *) at->raw != atWas);

	// ---- arrays ------------------------------------------------------------
	Array *array = newArray(64);
	check("an array knows its size", rawArraySize(array->raw) == 64);
	for (size_t i = 0; i < 64; i++) {
		array->raw->vars[i] = tagInt((intptr_t) i * 7);
	}
	arrayAtPutObject(array, 3, (Object *) hello);
	collectorScavenge(&heap);
	check("array elements survive a collection",
		array->raw->vars[0] == tagInt(0) && array->raw->vars[63] == tagInt(441));
	check("an object element is updated when it moves",
		rawStringEqualsBytes((RawString *) asObject(array->raw->vars[3]), "hello", 5));

	// ---- ordered collections ----------------------------------------------
	OrderedCollection *list = newOrdColl(4);
	check("a fresh collection is empty", ordCollSize(list) == 0);
	for (int i = 0; i < 500; i++) {
		ordCollAdd(list, tagInt(i));
	}
	check("appending past the initial capacity grows it",
		ordCollSize(list) == 500);
	int intact = 1;
	for (int i = 0; i < 500; i++) {
		intact = intact && ordCollAt(list, (size_t) i) == tagInt(i);
	}
	check("every appended element is where it belongs", intact);

	// Objects, so growth has to happen with a live pointer in flight: this is
	// the case where forming the tagged pointer BEFORE the grow would store an
	// address the object no longer has.
	OrderedCollection *objects = newOrdColl(4);
	for (int i = 0; i < 300; i++) {
		snprintf(buffer, sizeof(buffer), "item%d", i);
		ordCollAddObject(objects, (Object *) stringFromC(buffer));
	}
	collectorScavenge(&heap);
	int allGood = 1;
	for (int i = 0; i < 300; i++) {
		snprintf(buffer, sizeof(buffer), "item%d", i);
		Object *element = ordCollObjectAt(objects, (size_t) i);
		allGood = allGood
			&& rawStringEqualsBytes((RawString *) element->raw, buffer, strlen(buffer));
	}
	check("objects appended across a grow are intact after a collection", allGood);

	Array *asArray = ordCollAsArray(list);
	check("asArray copies the live span in order",
		rawArraySize(asArray->raw) == 500
		&& asArray->raw->vars[0] == tagInt(0)
		&& asArray->raw->vars[499] == tagInt(499));

	closeHandleScope(&outer, NULL);
	printf("\n%d of %d checks passed\n", gChecks - gFailures, gChecks);
	return gFailures == 0 ? 0 : 1;
}
