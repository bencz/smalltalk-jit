#include "tools/Snapshot.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "core/ClassTable.h"
#include "core/Endian.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "core/Thread.h"
#include "jit/CompiledMethod.h"
#include "jit/Jit.h"
#include "memory/Heap.h"
#include "memory/ObjectWalk.h"
#include "runtime/Collection.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The record layout
// ---------------------------------------------------------------------------
//
//   uint64 objectCount
//   uint64 unitCount
//   uint64 classTableSize
//   uint64 handleSlotCount
//   objectCount   x object record
//   unitCount     x unit record
//   classTableSize x uint64 object reference   (index -> class, 0 = free slot)
//   handleSlotCount x uint64 object reference  (SmalltalkHandles, positional)
//
// An OBJECT REFERENCE is `id + 1`, so 0 means "none" and can never collide with
// object 0.
//
// An object record is
//
//   uint64 header      the live header with the GC bits cleared
//   uint64 sizeBytes   total size including the header
//   body               sizeBytes - 8 bytes, verbatim except that every TAGGED
//                      slot holds an ENCODED VALUE (below)
//
// `sizeBytes` is written even though the header usually carries it, because it
// is the one thing the loader cannot derive: a size too large for the header's
// 8-bit field falls back to the class, and no class exists yet during the pass
// that allocates. Everything else the loader needs -- where an object's tagged
// slots are -- it asks objectPointerSlots for AFTER the class table is back.
//
// An ENCODED VALUE keeps the two tag bits, so it is self-describing: tag
// VALUE_POINTER means the rest is `id + 1`, and every other tag is an immediate
// written verbatim. Nothing else can be mistaken for a reference, because the
// three immediate tags are 00, 10 and 11. A slot holding the allocator's ZERO
// (the VM's "absent", core/Object.h) is tag 00 and survives as itself.

#define SNAPSHOT_NO_REF 0

typedef struct {
	FILE *file;
	int failed;

	// Write side: object address -> id, and the objects in id order.
	struct { void **keys; size_t *values; size_t capacity; size_t count; } ids;
	RawObject **objects;
	size_t objectCount;
	size_t objectCapacity;

	struct { void **keys; size_t *values; size_t capacity; size_t count; } unitIds;
	CodeUnit **units;
	size_t unitCount;
	size_t unitCapacity;

	// Read side: id -> address.
	RawObject **byId;
	CodeUnit **unitById;
} Snapshot;


// ---------------------------------------------------------------------------
// A pointer-keyed map, open addressing. Small enough to keep here rather than
// depending on a heap collection, which is the thing this file must not use:
// every key is a RAW ADDRESS and a collection would move the object out from
// under it.
// ---------------------------------------------------------------------------

static size_t pointerHash(void *key)
{
	// Fibonacci hashing on the address, shifted past the 16-byte alignment that
	// every heap object has (core/Object.h) so the low bits are not all zero.
	uint64_t bits = (uint64_t) (uintptr_t) key >> 4;
	return (size_t) ((bits * 0x9E3779B97F4A7C15ull) >> 32);
}


#define MAP_FIELDS(map) (map).keys, (map).values, (map).capacity

static size_t mapProbe(void **keys, size_t capacity, void *key)
{
	size_t index = pointerHash(key) & (capacity - 1);
	while (keys[index] != NULL && keys[index] != key) {
		index = (index + 1) & (capacity - 1);
	}
	return index;
}


static void mapInit(void ***keys, size_t **values, size_t *capacity, size_t *count)
{
	*capacity = 4096;
	*count = 0;
	*keys = calloc(*capacity, sizeof(void *));
	*values = calloc(*capacity, sizeof(size_t));
	ASSERT(*keys != NULL && *values != NULL);
}


static void mapGrow(void ***keys, size_t **values, size_t *capacity)
{
	size_t oldCapacity = *capacity;
	void **oldKeys = *keys;
	size_t *oldValues = *values;
	*capacity = oldCapacity * 2;
	*keys = calloc(*capacity, sizeof(void *));
	*values = calloc(*capacity, sizeof(size_t));
	ASSERT(*keys != NULL && *values != NULL);
	for (size_t i = 0; i < oldCapacity; i++) {
		if (oldKeys[i] == NULL) {
			continue;
		}
		size_t index = mapProbe(*keys, *capacity, oldKeys[i]);
		(*keys)[index] = oldKeys[i];
		(*values)[index] = oldValues[i];
	}
	free(oldKeys);
	free(oldValues);
}


// The id `key` already has, or SIZE_MAX when it has none.
static size_t mapAt(void **keys, size_t *values, size_t capacity, void *key)
{
	size_t index = mapProbe(keys, capacity, key);
	return keys[index] == NULL ? SIZE_MAX : values[index];
}


// ---------------------------------------------------------------------------
// Writing: discovery
// ---------------------------------------------------------------------------

static size_t objectId(Snapshot *snapshot, RawObject *object, _Bool *isNew)
{
	*isNew = 0;
	if (snapshot->ids.count * 2 >= snapshot->ids.capacity) {
		mapGrow(&snapshot->ids.keys, &snapshot->ids.values, &snapshot->ids.capacity);
	}
	size_t index = mapProbe(snapshot->ids.keys, snapshot->ids.capacity, object);
	if (snapshot->ids.keys[index] != NULL) {
		return snapshot->ids.values[index];
	}
	size_t id = snapshot->objectCount;
	snapshot->ids.keys[index] = object;
	snapshot->ids.values[index] = id;
	snapshot->ids.count++;
	if (snapshot->objectCount == snapshot->objectCapacity) {
		snapshot->objectCapacity *= 2;
		snapshot->objects = realloc(snapshot->objects,
			snapshot->objectCapacity * sizeof(RawObject *));
		ASSERT(snapshot->objects != NULL);
	}
	snapshot->objects[snapshot->objectCount++] = object;
	*isNew = 1;
	return id;
}


static size_t unitId(Snapshot *snapshot, CodeUnit *unit)
{
	if (snapshot->unitIds.count * 2 >= snapshot->unitIds.capacity) {
		mapGrow(&snapshot->unitIds.keys, &snapshot->unitIds.values,
			&snapshot->unitIds.capacity);
	}
	size_t index = mapProbe(snapshot->unitIds.keys, snapshot->unitIds.capacity, unit);
	if (snapshot->unitIds.keys[index] != NULL) {
		return snapshot->unitIds.values[index];
	}
	size_t id = snapshot->unitCount;
	snapshot->unitIds.keys[index] = unit;
	snapshot->unitIds.values[index] = id;
	snapshot->unitIds.count++;
	if (snapshot->unitCount == snapshot->unitCapacity) {
		snapshot->unitCapacity *= 2;
		snapshot->units = realloc(snapshot->units,
			snapshot->unitCapacity * sizeof(CodeUnit *));
		ASSERT(snapshot->units != NULL);
	}
	snapshot->units[snapshot->unitCount++] = unit;
	return id;
}


static void discoverValue(Snapshot *snapshot, Value value)
{
	if (valueTypeOf(value, VALUE_POINTER)) {
		_Bool isNew;
		objectId(snapshot, asObject(value), &isNew);
	}
}


static _Bool isCompiledMethod(RawObject *object)
{
	return rawObjectClassIndex(object) == classIndexOf(&Handles.CompiledMethod);
}


// Everything one object names. Two sources and not one: the tagged slots the
// collector already knows how to find, and -- for a CompiledMethod alone -- the
// CodeUnit hanging off a raw word, which is exactly the reference no collector
// walk would ever reach.
static void discoverFromObject(Snapshot *snapshot, RawObject *object)
{
	ClassTable *classes = &CurrentThread.heap->classes;
	size_t count;
	Value *slots = objectPointerSlots(classes, object, &count);
	for (size_t i = 0; i < count; i++) {
		discoverValue(snapshot, slots[i]);
	}
	if (isCompiledMethod(object) && ((RawCompiledMethod *) object)->unit != NULL) {
		unitId(snapshot, ((RawCompiledMethod *) object)->unit);
	}
}


static void discoverFromUnit(Snapshot *snapshot, CodeUnit *unit)
{
	discoverValue(snapshot, unit->literals);
	discoverValue(snapshot, unit->blocks);
	discoverValue(snapshot, unit->selector);
	discoverValue(snapshot, unit->ownerClass);
}


// ---------------------------------------------------------------------------
// Writing: the stream
// ---------------------------------------------------------------------------

static void writeWord(Snapshot *snapshot, uint64_t word)
{
	if (fwrite(&word, sizeof word, 1, snapshot->file) != 1) {
		snapshot->failed = 1;
	}
}


static void writeBytes(Snapshot *snapshot, const void *bytes, size_t count)
{
	if (count > 0 && fwrite(bytes, 1, count, snapshot->file) != count) {
		snapshot->failed = 1;
	}
}


static uint64_t encodedValue(Snapshot *snapshot, Value value)
{
	if (!valueTypeOf(value, VALUE_POINTER)) {
		return (uint64_t) value; // an immediate, verbatim, tag included
	}
	size_t id = mapAt(MAP_FIELDS(snapshot->ids), asObject(value));
	ASSERT(id != SIZE_MAX); // discovery ran first, so every reference has an id
	return ((uint64_t) (id + 1) << 2) | VALUE_POINTER;
}


static uint64_t objectRef(Snapshot *snapshot, RawObject *object)
{
	if (object == NULL) {
		return SNAPSHOT_NO_REF;
	}
	size_t id = mapAt(MAP_FIELDS(snapshot->ids), object);
	ASSERT(id != SIZE_MAX);
	return id + 1;
}


static void writeObjectRecord(Snapshot *snapshot, RawObject *object)
{
	ClassTable *classes = &CurrentThread.heap->classes;
	size_t sizeBytes = objectSizeInBytes(classes, object);

	// The GC bits are the collector's working state for THIS process (marked,
	// remembered, aged) and mean nothing in the next one. Writing them would hand
	// the loaded heap a set of objects that claim to be already marked.
	writeWord(snapshot, object->header & ~((uint64_t) 0x7F << OBJ_GC_SHIFT));
	writeWord(snapshot, sizeBytes);

	size_t bodyBytes = sizeBytes - HEADER_SIZE;
	size_t pointerCount;
	Value *slots = objectPointerSlots(classes, object, &pointerCount);
	size_t pointerStart = (size_t) ((uint8_t *) slots - object->body) / sizeof(Value);

	// The body goes out verbatim except for the tagged run, which is the only
	// part that holds addresses. Raw words -- an element count, a shape trailer,
	// a run of bytes, a double -- mean the same in any process.
	uint8_t *copy = malloc(bodyBytes);
	ASSERT(copy != NULL);
	memcpy(copy, object->body, bodyBytes);
	for (size_t i = 0; i < pointerCount; i++) {
		((uint64_t *) copy)[pointerStart + i] = encodedValue(snapshot, slots[i]);
	}
	if (isCompiledMethod(object)) {
		// The two raw words that are C pointers. `unit` becomes a unit reference;
		// `native` becomes nothing, and the method compiles again on its first
		// send. Keeping it would be a jump into an address that belonged to the
		// process that wrote the image.
		RawCompiledMethod *method = (RawCompiledMethod *) object;
		uint64_t *raw = (uint64_t *) copy;
		raw[1] = method->unit == NULL ? SNAPSHOT_NO_REF
			: mapAt(MAP_FIELDS(snapshot->unitIds), method->unit) + 1;
		raw[2] = 0;
	}
	writeBytes(snapshot, copy, bodyBytes);
	free(copy);
}


static void writeUnitRecord(Snapshot *snapshot, CodeUnit *unit)
{
	writeWord(snapshot, unit->instructionCount);
	writeWord(snapshot, unit->registerCount);
	writeWord(snapshot, unit->argumentCount);
	writeWord(snapshot, unit->captureCount);
	writeWord(snapshot, unit->primitive);
	writeWord(snapshot, (uint64_t) unit->isBlock | ((uint64_t) unit->couldBeHome << 1));
	writeWord(snapshot, encodedValue(snapshot, unit->literals));
	writeWord(snapshot, encodedValue(snapshot, unit->blocks));
	writeWord(snapshot, encodedValue(snapshot, unit->selector));
	writeWord(snapshot, encodedValue(snapshot, unit->ownerClass));
	writeBytes(snapshot, unit->code, unit->instructionCount * sizeof(Instruction));

	// The side tables are OPTIONAL and every one of them is absent today: the
	// front end allocates none of them. Written with a presence word rather than
	// assumed away, so filling one in later is a change to this file and not a
	// silently truncated image.
	writeWord(snapshot, unit->sourcePositions != NULL);
	if (unit->sourcePositions != NULL) {
		writeBytes(snapshot, unit->sourcePositions,
			unit->instructionCount * sizeof(uint32_t));
	}
	writeWord(snapshot, unit->declaredTypes != NULL);
	if (unit->declaredTypes != NULL) {
		writeBytes(snapshot, unit->declaredTypes, unit->registerCount);
	}
	writeWord(snapshot, unit->declaredClass != NULL);
	if (unit->declaredClass != NULL) {
		writeBytes(snapshot, unit->declaredClass,
			unit->registerCount * sizeof(uint16_t));
	}
	writeWord(snapshot, unit->returnType);
}


void snapshotWriteHeader(FILE *file)
{
	uint8_t header[8];
	memcpy(header, SNAPSHOT_MAGIC, 4);
	header[4] = SNAPSHOT_FORMAT_VERSION;
	header[5] = TARGET_BIG_ENDIAN ? SNAPSHOT_BYTE_ORDER_BIG : SNAPSHOT_BYTE_ORDER_LITTLE;
	header[6] = sizeof(Value);
	header[7] = 0;
	size_t written = fwrite(header, 1, sizeof header, file);
	ASSERT(written == sizeof header);
}


int snapshotCheckHeader(FILE *file, char *err, size_t errSize)
{
	uint8_t header[8];
	if (fread(header, 1, sizeof header, file) != sizeof header
			|| memcmp(header, SNAPSHOT_MAGIC, 4) != 0) {
		snprintf(err, errSize, "not an image (no '%s' magic): legacy or corrupt, "
			"regenerate it with `st -s <image> -b packages/Core`", SNAPSHOT_MAGIC);
		return -1;
	}
	if (header[4] != SNAPSHOT_FORMAT_VERSION) {
		snprintf(err, errSize, "image format v%d, this VM reads v%d: re-bootstrap "
			"it with `st -s <image> -b packages/Core`",
			header[4], SNAPSHOT_FORMAT_VERSION);
		return -1;
	}
	uint8_t order = TARGET_BIG_ENDIAN
		? SNAPSHOT_BYTE_ORDER_BIG : SNAPSHOT_BYTE_ORDER_LITTLE;
	if (header[5] != order) {
		snprintf(err, errSize, "image is %s-endian and this host is %s-endian: "
			"an image is a per-build artifact, re-bootstrap it here",
			header[5] == SNAPSHOT_BYTE_ORDER_BIG ? "big" : "little",
			TARGET_BIG_ENDIAN ? "big" : "little");
		return -1;
	}
	if (header[6] != sizeof(Value)) {
		snprintf(err, errSize, "image word size %d, host word size %zu: re-bootstrap",
			header[6], sizeof(Value));
		return -1;
	}
	return 0;
}


int snapshotWrite(FILE *file)
{
	Heap *heap = CurrentThread.heap;
	Snapshot snapshot;
	memset(&snapshot, 0, sizeof snapshot);
	snapshot.file = file;
	mapInit(&snapshot.ids.keys, &snapshot.ids.values, &snapshot.ids.capacity,
		&snapshot.ids.count);
	mapInit(&snapshot.unitIds.keys, &snapshot.unitIds.values,
		&snapshot.unitIds.capacity, &snapshot.unitIds.count);
	snapshot.objectCapacity = 4096;
	snapshot.objects = malloc(snapshot.objectCapacity * sizeof(RawObject *));
	snapshot.unitCapacity = 1024;
	snapshot.units = malloc(snapshot.unitCapacity * sizeof(CodeUnit *));
	ASSERT(snapshot.objects != NULL && snapshot.units != NULL);

	// No collection from here to the last byte. Every key in the maps above is a
	// RAW ADDRESS, so a collection that moved one object would leave the whole
	// table pointing at where things used to be. Nothing below allocates; the
	// inhibit is what makes that a checked claim instead of a hope.
	heapInhibitGc(heap, 1);

	// THE ROOTS ARE TWO. The well-known handles reach nil, true, false, every
	// kernel class, the symbol table and the system dictionary, which is every
	// global. The CLASS TABLE reaches what none of that does: a metaclass is
	// named by an object header's index and lives nowhere else.
	// A FLAT SLOT WALK, the same one the collector does
	// (smalltalkHandlesVisitRoots): a well-known class added to the struct is
	// then in the image automatically instead of being left out until someone
	// notices. Every field is exactly one pointer wide and that is asserted where
	// the struct is declared.
	RawObject **handle = (RawObject **) heap->handles;
	size_t handleSlots = smalltalkHandleSlotCount();
	for (size_t i = 0; i < handleSlots; i++) {
		if (handle[i] != NULL) {
			_Bool isNew;
			objectId(&snapshot, handle[i], &isNew);
		}
	}
	for (size_t i = CLASS_INDEX_FIRST; i < heap->classes.size; i++) {
		RawObject *entry = heap->classes.entries[i];
		if (classTableIsLive(entry)) {
			_Bool isNew;
			objectId(&snapshot, entry, &isNew);
		}
	}

	// Discovery, over two growing lists rather than by recursion: the graph is
	// cyclic and as deep as the class hierarchy times the method dictionaries,
	// and a recursive walk would put that on the C stack.
	size_t objectCursor = 0;
	size_t unitCursor = 0;
	while (objectCursor < snapshot.objectCount || unitCursor < snapshot.unitCount) {
		while (objectCursor < snapshot.objectCount) {
			discoverFromObject(&snapshot, snapshot.objects[objectCursor++]);
		}
		while (unitCursor < snapshot.unitCount) {
			discoverFromUnit(&snapshot, snapshot.units[unitCursor++]);
		}
	}

	snapshotWriteHeader(file);
	writeWord(&snapshot, snapshot.objectCount);
	writeWord(&snapshot, snapshot.unitCount);
	writeWord(&snapshot, heap->classes.size);
	writeWord(&snapshot, handleSlots);
	for (size_t i = 0; i < snapshot.objectCount; i++) {
		writeObjectRecord(&snapshot, snapshot.objects[i]);
	}
	for (size_t i = 0; i < snapshot.unitCount; i++) {
		writeUnitRecord(&snapshot, snapshot.units[i]);
	}
	for (size_t i = 0; i < heap->classes.size; i++) {
		RawObject *entry = heap->classes.entries[i];
		writeWord(&snapshot, classTableIsLive(entry)
			? objectRef(&snapshot, entry) : SNAPSHOT_NO_REF);
	}
	for (size_t i = 0; i < handleSlots; i++) {
		writeWord(&snapshot, objectRef(&snapshot, handle[i]));
	}

	heapInhibitGc(heap, 0);
	free(snapshot.ids.keys);
	free(snapshot.ids.values);
	free(snapshot.unitIds.keys);
	free(snapshot.unitIds.values);
	free(snapshot.objects);
	free(snapshot.units);
	return snapshot.failed ? -1 : 0;
}


// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

static uint64_t readWord(Snapshot *snapshot)
{
	uint64_t word = 0;
	if (fread(&word, sizeof word, 1, snapshot->file) != 1) {
		snapshot->failed = 1;
	}
	return word;
}


static void readBytes(Snapshot *snapshot, void *into, size_t count)
{
	if (count > 0 && fread(into, 1, count, snapshot->file) != count) {
		snapshot->failed = 1;
	}
}


static Value decodedValue(Snapshot *snapshot, uint64_t encoded)
{
	if ((encoded & 3) != VALUE_POINTER) {
		return (Value) encoded;
	}
	size_t id = (size_t) (encoded >> 2);
	ASSERT(id >= 1 && id - 1 < snapshot->objectCount);
	return tagPtr(snapshot->byId[id - 1]);
}


static RawObject *refObject(Snapshot *snapshot, uint64_t ref)
{
	if (ref == SNAPSHOT_NO_REF) {
		return NULL;
	}
	ASSERT(ref - 1 < snapshot->objectCount);
	return snapshot->byId[ref - 1];
}


int snapshotRead(FILE *file)
{
	Heap *heap = CurrentThread.heap;
	Snapshot snapshot;
	memset(&snapshot, 0, sizeof snapshot);
	snapshot.file = file;

	char err[256];
	if (snapshotCheckHeader(file, err, sizeof err) != 0) {
		fprintf(stderr, "st: %s\n", err);
		return -1;
	}

	snapshot.objectCount = (size_t) readWord(&snapshot);
	snapshot.unitCount = (size_t) readWord(&snapshot);
	size_t classTableSize = (size_t) readWord(&snapshot);
	size_t handleSlots = (size_t) readWord(&snapshot);
	if (snapshot.failed) {
		fprintf(stderr, "st: image is truncated in its header\n");
		return -1;
	}
	if (handleSlots != smalltalkHandleSlotCount()) {
		// Positional, so a struct that grew or shrank cannot be read at all. It
		// says so instead of restoring the wrong object into every field after
		// the one that moved.
		fprintf(stderr, "st: image has %zu well-known slots, this VM has %zu: "
			"re-bootstrap it with `st -s <image> -b packages/Core`\n",
			handleSlots, smalltalkHandleSlotCount());
		return -1;
	}

	snapshot.byId = calloc(snapshot.objectCount, sizeof(RawObject *));
	snapshot.unitById = calloc(snapshot.unitCount == 0 ? 1 : snapshot.unitCount,
		sizeof(CodeUnit *));
	ASSERT(snapshot.byId != NULL && snapshot.unitById != NULL);

	// Same reason as the write side, and a stronger one: until the fixup pass
	// below finishes, the objects read so far are reachable ONLY from `byId`,
	// which is a C array no root provider knows about.
	heapInhibitGc(heap, 1);

	// PASS 1: allocate and fill bodies verbatim. Nothing is decoded yet, because
	// decoding a reference needs every id to have an address and the last object
	// has not been read.
	for (size_t i = 0; i < snapshot.objectCount && !snapshot.failed; i++) {
		uint64_t header = readWord(&snapshot);
		size_t sizeBytes = (size_t) readWord(&snapshot);
		if (snapshot.failed || sizeBytes < HEADER_SIZE
				|| sizeBytes != objectAlignSize(sizeBytes)) {
			snapshot.failed = 1;
			break;
		}
		RawObject *object = (RawObject *) heapAllocateImageBytes(heap, sizeBytes);
		// The old space, so bit 3 of the address is clear and the object reads as
		// OLD. An image that landed in the nursery would be moved by the first
		// collection, and the addresses generated code bakes for nil, true and
		// false would stop matching (jitCompileFor asserts on exactly this).
		ASSERT(isOldObject(object));
		object->header = header;
		readBytes(&snapshot, object->body, sizeBytes - HEADER_SIZE);
		snapshot.byId[i] = object;
	}

	// PASS 2: the units, whose Values stay encoded for the moment.
	for (size_t i = 0; i < snapshot.unitCount && !snapshot.failed; i++) {
		CodeUnit *unit = calloc(1, sizeof(CodeUnit));
		ASSERT(unit != NULL);
		unit->instructionCount = (uint16_t) readWord(&snapshot);
		unit->registerCount = (uint16_t) readWord(&snapshot);
		unit->argumentCount = (uint16_t) readWord(&snapshot);
		unit->captureCount = (uint16_t) readWord(&snapshot);
		unit->primitive = (uint16_t) readWord(&snapshot);
		uint64_t flags = readWord(&snapshot);
		unit->isBlock = (flags & 1) != 0;
		unit->couldBeHome = (flags & 2) != 0;
		unit->literals = (Value) readWord(&snapshot);
		unit->blocks = (Value) readWord(&snapshot);
		unit->selector = (Value) readWord(&snapshot);
		unit->ownerClass = (Value) readWord(&snapshot);
		unit->code = calloc(unit->instructionCount == 0 ? 1 : unit->instructionCount,
			sizeof(Instruction));
		ASSERT(unit->code != NULL);
		readBytes(&snapshot, unit->code, unit->instructionCount * sizeof(Instruction));
		if (readWord(&snapshot)) {
			unit->sourcePositions = calloc(unit->instructionCount, sizeof(uint32_t));
			ASSERT(unit->sourcePositions != NULL);
			readBytes(&snapshot, unit->sourcePositions,
				unit->instructionCount * sizeof(uint32_t));
		}
		if (readWord(&snapshot)) {
			unit->declaredTypes = calloc(unit->registerCount, 1);
			ASSERT(unit->declaredTypes != NULL);
			readBytes(&snapshot, unit->declaredTypes, unit->registerCount);
		}
		if (readWord(&snapshot)) {
			unit->declaredClass = calloc(unit->registerCount, sizeof(uint16_t));
			ASSERT(unit->declaredClass != NULL);
			readBytes(&snapshot, unit->declaredClass,
				unit->registerCount * sizeof(uint16_t));
		}
		unit->returnType = (uint8_t) readWord(&snapshot);
		snapshot.unitById[i] = unit;
	}

	// PASS 3: the class table, index for index. This has to happen before the
	// fixup below, because finding an object's tagged slots goes through its
	// class for the two MIXED formats.
	classTableFree(&heap->classes);
	classTableInit(&heap->classes);
	for (size_t i = 0; i < classTableSize && !snapshot.failed; i++) {
		RawObject *class = refObject(&snapshot, readWord(&snapshot));
		if (class != NULL) {
			classTableSet(&heap->classes, (uint32_t) i, class);
		}
	}

	// PASS 4: the well-known handles, positionally. Needed before the fixup too,
	// which asks which class index a CompiledMethod has.
	RawObject **handle = (RawObject **) heap->handles;
	for (size_t i = 0; i < handleSlots && !snapshot.failed; i++) {
		handle[i] = refObject(&snapshot, readWord(&snapshot));
	}

	if (snapshot.failed) {
		fprintf(stderr, "st: image is truncated or corrupt\n");
		heapInhibitGc(heap, 0);
		free(snapshot.byId);
		free(snapshot.unitById);
		return -1;
	}

	// PASS 5: turn every encoded reference into an address. Objects first, then
	// units, then the raw word that binds a method to its unit.
	uint32_t methodClass = classIndexOf(&Handles.CompiledMethod);
	for (size_t i = 0; i < snapshot.objectCount; i++) {
		RawObject *object = snapshot.byId[i];
		size_t count;
		Value *slots = objectPointerSlots(&heap->classes, object, &count);
		for (size_t s = 0; s < count; s++) {
			slots[s] = decodedValue(&snapshot, (uint64_t) slots[s]);
		}
		if (rawObjectClassIndex(object) == methodClass) {
			RawCompiledMethod *method = (RawCompiledMethod *) object;
			uint64_t ref = (uint64_t) (uintptr_t) method->unit;
			ASSERT(ref == SNAPSHOT_NO_REF || ref - 1 < snapshot.unitCount);
			method->unit = ref == SNAPSHOT_NO_REF
				? NULL : snapshot.unitById[ref - 1];
			method->native = NULL; // compiled again on the first send
		}
	}
	for (size_t i = 0; i < snapshot.unitCount; i++) {
		CodeUnit *unit = snapshot.unitById[i];
		unit->literals = decodedValue(&snapshot, (uint64_t) unit->literals);
		unit->blocks = decodedValue(&snapshot, (uint64_t) unit->blocks);
		unit->selector = decodedValue(&snapshot, (uint64_t) unit->selector);
		unit->ownerClass = decodedValue(&snapshot, (uint64_t) unit->ownerClass);
		// On the registry, so the literal frame is a GC root from now on. A unit
		// is a malloc'd C struct holding tagged Values; without this the first
		// collection after the load moves exactly the objects it names and
		// nothing updates it (memory/Roots.h).
		unit->registered = 0;
		unit->nextUnit = NULL;
		jitRegisterUnit(unit);
	}

	// The symbol count is a cached number about the heap that just went away.
	heap->symbolCountValid = 0;
	heap->symbolCount = 0;

	heapInhibitGc(heap, 0);
	free(snapshot.byId);
	free(snapshot.unitById);
	return 0;
}
