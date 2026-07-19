#include "tools/Snapshot.h"
#include "core/Thread.h"
#include "memory/Heap.h"
#include "core/Object.h"
#include "memory/Heap.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "core/Assert.h"
#include "core/Endian.h"
#include <stdlib.h>
#include <string.h>

#define OBJECT_FIELD_MASK 7
#define OBJECT_INLINE 1
#define OBJECT_POINTER 5

enum {
	SS_ASSOC_DEFINED = 1,
	SS_ASSOC_WRITTEN = 1 << 1,
};

typedef struct {
	uint8_t flags;
	intptr_t key;
	intptr_t value;
} SnapshotAssoc;

typedef struct {
	size_t size;
	size_t tally;
	SnapshotAssoc *array;
} SnapshotDictionary;

typedef struct {
	FILE *file;
	SnapshotDictionary dict;
} Snapshot;

static void iterateHandles(Snapshot *snapshot);
static void writeNewObject(Snapshot *snapshot, RawObject *object);
static void writeObject(Snapshot *snapshot, RawObject *object);
static void writeField(Snapshot *snapshot, Value value);
static void writeFieldObject(Snapshot *snapshot, RawObject *object);
static void writeInt64(Snapshot *snapshot, int64_t value);
static void registerBuiltinObjects(Snapshot *snapshot);
static SnapshotAssoc *registerObject(Snapshot *snapshot, RawObject *object, _Bool written);
static Value readField(Snapshot *snapshot);
static Value readObject(int64_t field, Snapshot *snapshot);
static int64_t readInt64(Snapshot *snapshot);
static void createBuiltinObjectsHandles(Snapshot *snapshot);
static void initDicitonary(SnapshotDictionary *dict);
static void freeDictionary(SnapshotDictionary *dict);
static SnapshotAssoc *snapshotDictAtPut(SnapshotDictionary *dict, intptr_t key, intptr_t value);
static void snapshotGrowDict(SnapshotDictionary *dict);
static SnapshotAssoc *snapshotDictAt(SnapshotDictionary *dict, intptr_t key);
static ptrdiff_t findIndex(SnapshotDictionary *dict, intptr_t key);


// ---- format primitives ------------------------------------------------------

uint64_t snapshotEncodeShape(InstanceShape shape)
{
	return (uint64_t) shape.tag
		| (uint64_t) shape.payloadSize << 8
		| (uint64_t) shape.varsSize << 16
		| (uint64_t) shape.isIndexed << 24
		| (uint64_t) shape.isBytes << 32
		| (uint64_t) shape.valueType << 40
		| (uint64_t) shape.size << 48;
}


InstanceShape snapshotDecodeShape(uint64_t bits)
{
	InstanceShape shape;
	shape.tag = (uint8_t) bits;
	shape.payloadSize = (uint8_t) (bits >> 8);
	shape.varsSize = (uint8_t) (bits >> 16);
	shape.isIndexed = (uint8_t) (bits >> 24);
	shape.isBytes = (uint8_t) (bits >> 32);
	shape.valueType = (uint8_t) (bits >> 40);
	shape.size = (uint16_t) (bits >> 48);
	return shape;
}


uint64_t snapshotEncodeObjectHeader(uint32_t hash, uint8_t payloadSize, uint8_t varsSize)
{
	return (uint64_t) hash
		| (uint64_t) payloadSize << 40
		| (uint64_t) varsSize << 48;
}


void snapshotDecodeObjectHeader(uint64_t bits, uint32_t *hash, uint8_t *payloadSize, uint8_t *varsSize)
{
	*hash = (uint32_t) bits;
	*payloadSize = (uint8_t) (bits >> 40);
	*varsSize = (uint8_t) (bits >> 48);
}


void snapshotWriteHeader(FILE *file)
{
	uint8_t header[8];
	memcpy(header, SNAPSHOT_MAGIC, 4);
	header[4] = SNAPSHOT_FORMAT_VERSION;
	header[5] = TARGET_BIG_ENDIAN ? SNAPSHOT_BYTE_ORDER_BIG : SNAPSHOT_BYTE_ORDER_LITTLE;
	header[6] = sizeof(Value);
	header[7] = 0;
	size_t written = fwrite(header, 1, sizeof(header), file);
	ASSERT(written == sizeof(header));
}


int snapshotCheckHeader(FILE *file, char *err, size_t errSize)
{
	uint8_t header[8];
	if (fread(header, 1, sizeof(header), file) != sizeof(header)
		|| memcmp(header, SNAPSHOT_MAGIC, 4) != 0) {
		snprintf(err, errSize,
			"not a valid image (missing '%s' magic): legacy or corrupt snapshot - "
			"regenerate it with `st -s <image> -b smalltalk`", SNAPSHOT_MAGIC);
		return -1;
	}
	if (header[4] != SNAPSHOT_FORMAT_VERSION) {
		snprintf(err, errSize,
			"image format v%d, this VM reads v%d - re-bootstrap the image",
			header[4], SNAPSHOT_FORMAT_VERSION);
		return -1;
	}
	uint8_t hostOrder = TARGET_BIG_ENDIAN ? SNAPSHOT_BYTE_ORDER_BIG : SNAPSHOT_BYTE_ORDER_LITTLE;
	if (header[5] != hostOrder) {
		snprintf(err, errSize,
			"image was built %s-endian but this host is %s-endian: images are "
			"per-build artifacts - re-bootstrap on this host",
			header[5] == SNAPSHOT_BYTE_ORDER_BIG ? "big" : "little",
			hostOrder == SNAPSHOT_BYTE_ORDER_BIG ? "big" : "little");
		return -1;
	}
	if (header[6] != sizeof(Value)) {
		snprintf(err, errSize, "image word size %d, host word size %zu - re-bootstrap",
			header[6], sizeof(Value));
		return -1;
	}
	return 0;
}


void snapshotWrite(FILE *file)
{
	Snapshot snapshot;
	snapshot.file = file;
	snapshotWriteHeader(file);
	initDicitonary(&snapshot.dict);
	registerBuiltinObjects(&snapshot);
	iterateHandles(&snapshot);
	freeDictionary(&snapshot.dict);
}


static _Bool isFloatShape(InstanceShape shape)
{
	/* A boxed Float carries its double in a single unscanned payload word and
	   has no pointer/indexed slots. This is detectable from the (serialized)
	   shape alone, so it works during read when Handles is not yet populated. */
	return shape.payloadSize == 1 && shape.varsSize == 0 && !shape.isIndexed;
}


static void iterateHandles(Snapshot *snapshot)
{
	HandlesIterator handlesIterator;
	initHandlesIterator(&handlesIterator, CurrentThread.handles);
	while (handlesIteratorHasNext(&handlesIterator)) {
		writeNewObject(snapshot, handlesIteratorNext(&handlesIterator)->raw);
	}

	HandleScopeIterator handleScopeIterator;
	initHandleScopeIterator(&handleScopeIterator, CurrentThread.handleScopes);
	while (handleScopeIteratorHasNext(&handleScopeIterator)) {
		HandleScope *scope = handleScopeIteratorNext(&handleScopeIterator);
		for (size_t i = 0; i < scope->size; i++) {
			writeNewObject(snapshot, handleScopeAt(scope, i)->raw);
		}
	}
}


static void writeNewObject(Snapshot *snapshot, RawObject *object)
{
	SnapshotAssoc *id = snapshotDictAt(&snapshot->dict, (intptr_t) object);
	if (id == NULL || (id->flags & SS_ASSOC_WRITTEN) == 0) {
		writeObject(snapshot, object);
	}
}


// +------------------------------------+
// | ID | pointer or inline | value tag |
// | instance shape                     |
// | object header                      |
// | indexed size (optional)            |
// | class                              |
// +------------------------------------+
static void writeObject(Snapshot *snapshot, RawObject *object)
{
	SnapshotAssoc *id = registerObject(snapshot, object, 1);
	Value *vars = getRawObjectVars(object);
	size_t size = object->class->instanceShape.varsSize;
	if (object->class->instanceShape.isIndexed && !object->class->instanceShape.isBytes) {
		size += rawObjectSize(object);
	}

	// The stream is native-endian and the image header declares which (the
	// loader refuses foreign images loudly) — but the SHAPE and OBJECT-HEADER
	// words use the explicit field encodings so the format never depends on
	// struct layout/padding. Bytecode byte-payloads still carry native-endian
	// int32/Value fields; that is covered by the same per-endianness stance.
	writeInt64(snapshot, (id->value << 3) | OBJECT_INLINE);
	writeInt64(snapshot, (int64_t) snapshotEncodeShape(object->class->instanceShape));
	writeInt64(snapshot, (int64_t) snapshotEncodeObjectHeader(object->hash, object->payloadSize, object->varsSize));
	if (object->class->instanceShape.isIndexed) {
		writeInt64(snapshot, rawObjectSize(object));
	}
	writeFieldObject(snapshot, (RawObject *) object->class);
	for (size_t i = 0; i < size; i++) {
		writeField(snapshot, vars[i]);
	}
	if (object->class->instanceShape.isIndexed && object->class->instanceShape.isBytes) {
		size_t written = fwrite(getRawObjectIndexedVars(object), sizeof(uint8_t), rawObjectSize(object), snapshot->file);
		ASSERT(written == rawObjectSize(object));
	}
	if (isFloatShape(object->class->instanceShape)) {
		/* the double lives in the (unscanned) payload word, which is not a
		   pointer field, so serialize its raw bits explicitly */
		int64_t floatBits;
		memcpy(&floatBits, &((RawFloat *) object)->value, sizeof(floatBits));
		writeInt64(snapshot, floatBits);
	}
}


static void writeField(Snapshot *snapshot, Value value)
{
	if (valueTypeOf(value, VALUE_POINTER)) {
		writeFieldObject(snapshot, asObject(value));
	} else {
		writeInt64(snapshot, value);
	}
}


static void writeFieldObject(Snapshot *snapshot, RawObject *object)
{
	SnapshotAssoc *id = snapshotDictAt(&snapshot->dict, (intptr_t) object);
	if (id == NULL || (id->flags & SS_ASSOC_WRITTEN) == 0) {
		writeObject(snapshot, object);
	} else {
		writeInt64(snapshot, (id->value << 3) | OBJECT_POINTER);
	}
}


static void writeInt64(Snapshot *snapshot, int64_t value)
{
	size_t written = fwrite(&value, sizeof(int64_t), 1, snapshot->file);
	ASSERT(written == 1);
}


static void registerBuiltinObjects(Snapshot *snapshot)
{
	Object **handle = (Object **) &Handles.nil;
	Object **end = handle + sizeof(Handles) / sizeof(*handle);
	int64_t position = 0;

	while (handle < end) {
		SnapshotAssoc *assoc = registerObject(snapshot, (*handle)->raw, 0);
		// The read side rebuilds Handles positionally (createBuiltinObjectsHandles:
		// field i <- snapshot ID i), which only holds while every Handles field is
		// a DISTINCT object: a duplicate raw collapses to one ID here and silently
		// shifts the mapping for every later field. Fail at image-write time.
		ASSERT(assoc->value == position);
		position++;
		handle++;
	}
}


static SnapshotAssoc *registerObject(Snapshot *snapshot, RawObject *object, _Bool written)
{
	SnapshotDictionary *dict = &snapshot->dict;
	ptrdiff_t index = findIndex(dict, (intptr_t) object);
	SnapshotAssoc *assoc = &dict->array[index];
	if ((assoc->flags & SS_ASSOC_DEFINED) == 0) {
		assoc->key = (intptr_t) object;
		assoc->value = dict->tally++;
		assoc->flags |= SS_ASSOC_DEFINED;
		if (dict->tally == dict->size) {
			snapshotGrowDict(dict);
			/* the backing array was reallocated: re-find the slot so `assoc`
			   does not dangle into freed memory */
			assoc = &dict->array[findIndex(dict, (intptr_t) object)];
		}
	}
	if (written) {
		assoc->flags |= SS_ASSOC_WRITTEN;
	}
	return assoc;
}


// AOT seam: `Snapshot.file` is the ONLY FILE* coupling of the read path
// (readObject/readField/readInt64 all funnel through fread on it). The
// planned embed-in-executable feature needs snapshotReadFromMemory(buffer,
// size); route these reads through a tiny reader indirection (file vs memory
// cursor) when building it, a mechanical refactor by design.
void snapshotRead(FILE *file)
{
	Snapshot snapshot;
	snapshot.file = file;

	char err[256];
	if (snapshotCheckHeader(file, err, sizeof(err)) != 0) {
		fprintf(stderr, "snapshot: %s\n", err);
		exit(EXIT_FAILURE);
	}

	initDicitonary(&snapshot.dict);

	do {
		int64_t field;
		if (fread(&field, sizeof(field), 1, file) != 1) {
			break;
		}
		readObject(field, &snapshot);
	} while (1);

	createBuiltinObjectsHandles(&snapshot);
	freeDictionary(&snapshot.dict);
}


static Value readField(Snapshot *snapshot)
{
	int64_t field = readInt64(snapshot);
	SnapshotAssoc *assoc;
	switch (field & OBJECT_FIELD_MASK) {
	case OBJECT_INLINE:
		return readObject(field, snapshot);
	case OBJECT_POINTER:
		assoc = snapshotDictAt(&snapshot->dict, field >> 3);
		ASSERT(assoc != NULL);
		return assoc->value;
	default:
		return field;
	}
}


static Value readObject(int64_t field, Snapshot *snapshot)
{
	ASSERT((field & OBJECT_FIELD_MASK) == OBJECT_INLINE);

	int64_t header[2];
	size_t read = fread(header, sizeof(int64_t), 2, snapshot->file);
	ASSERT(read == 2);

	uint64_t id = field >> 3;
	InstanceShape shape = snapshotDecodeShape((uint64_t) header[0]);
	size_t size = shape.varsSize;
	size_t indexedSize = shape.isIndexed ? readInt64(snapshot) : 0;

	RawObject *object = (RawObject *) allocate(CurrentThread.heap, computeInstanceSize(shape, indexedSize));
	snapshotDictAtPut(&snapshot->dict, id, tagPtr(object));

	if (shape.isIndexed) {
		((RawIndexedObject *) object)->size = indexedSize;
		if (!shape.isBytes) {
			size += indexedSize;
		}
	}

	uint32_t hash;
	uint8_t payloadSize, varsSize;
	snapshotDecodeObjectHeader((uint64_t) header[1], &hash, &payloadSize, &varsSize);
	object->hash = hash;
	object->payloadSize = payloadSize;
	object->varsSize = varsSize;
	object->unused = 0;
	object->tags = 0;
	object->class = (RawClass *) asObject(readField(snapshot));
	ASSERT(object->class != NULL);

	Value *vars = getRawObjectVarsFromShape(object, shape);
	for (size_t i = 0; i < size; i++) {
		vars[i] = readField(snapshot);
	}

	if (shape.isIndexed && shape.isBytes) {
		size_t numRead = fread(getRawObjectIndexedVarsFromShape(object, shape), sizeof(uint8_t), indexedSize, snapshot->file);
	}
	if (isFloatShape(shape)) {
		int64_t bits = readInt64(snapshot);
		memcpy(&((RawFloat *) object)->value, &bits, sizeof(bits));
	}

	return tagPtr(object);
}


static int64_t readInt64(Snapshot *snapshot)
{
	int64_t value;
	size_t read = fread(&value, sizeof(value), 1, snapshot->file);
	ASSERT(read == 1);
	return value;
}


static void createBuiltinObjectsHandles(Snapshot *snapshot)
{
	Object **object = (Object **) &Handles.nil;
	Object **end = object + sizeof(Handles) / sizeof(*object);
	size_t i = 0;

	while (object < end) {
		*object = handle(asObject(snapshotDictAt(&snapshot->dict, i++)->value));
		object++;
	}
}


static void initDicitonary(SnapshotDictionary *dict)
{
	dict->size = 1024 * 8;
	dict->tally = 0;
	dict->array = malloc(dict->size * sizeof(*dict->array));
	ASSERT(dict->array != NULL);
	memset(dict->array, 0, dict->size * sizeof(*dict->array));
}


static void freeDictionary(SnapshotDictionary *dict)
{
	free(dict->array);
}


static SnapshotAssoc *snapshotDictAtPut(SnapshotDictionary *dict, intptr_t key, intptr_t value)
{
	ASSERT(dict->tally < dict->size);
	ptrdiff_t index = findIndex(dict, key);
	SnapshotAssoc *assoc = &dict->array[index];
	assoc->key = key;
	assoc->value = value;
	assoc->flags |= SS_ASSOC_DEFINED;
	dict->tally++;
	if (dict->tally == dict->size) {
		snapshotGrowDict(dict);
		assoc = &dict->array[findIndex(dict, key)];
	}
	return assoc;
}


static void snapshotGrowDict(SnapshotDictionary *dict)
{
	size_t newSize = dict->size * 2;
	SnapshotAssoc *newArray = malloc(newSize * sizeof(*dict->array));
	ASSERT(newArray != NULL);
	memset(newArray, 0, newSize * sizeof(*dict->array));

	SnapshotAssoc *array = dict->array;
	dict->array = newArray;
	dict->size = newSize;

	for (size_t i = 0; i < dict->tally; i++) {
		if (array[i].flags & SS_ASSOC_DEFINED) {
			memcpy(&newArray[findIndex(dict, array[i].key)], &array[i], sizeof(*array));
		}
	}

	free(array);
}


static SnapshotAssoc *snapshotDictAt(SnapshotDictionary *dict, intptr_t key)
{
	ASSERT(dict->tally < dict->size);
	ptrdiff_t index = findIndex(dict, key);
	SnapshotAssoc *assoc = &dict->array[index];
	return (assoc->flags & SS_ASSOC_DEFINED) == 0 ? NULL : assoc;
}


static ptrdiff_t findIndex(SnapshotDictionary *dict, intptr_t key)
{
	ASSERT(dict->tally < dict->size);
	ptrdiff_t index = key & (dict->size - 1);

	do {
		SnapshotAssoc *assoc = &dict->array[index];
		if ((assoc->flags & SS_ASSOC_DEFINED) == 0 || assoc->key == key) {
			return index;
		}
		index = index == dict->size - 1 ? 0 : index + 1;
	} while(1);
}
