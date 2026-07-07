#include "Message.h"
#include "Heap.h"
#include "Handle.h"
#include "Thread.h"
#include "Smalltalk.h"
#include "String.h"
#include "Class.h"
#include "Entry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Wire tags. Objects are numbered 0,1,2,... in DFS pre-order by BOTH sides, so a
// back-reference only needs to carry that sequential id.
enum {
	MSG_INT = 1,   // immediate (non-pointer) Value, verbatim int64
	MSG_NIL, MSG_TRUE, MSG_FALSE, // singletons -> destination's Handles.*
	MSG_SYMBOL,    // len + bytes -> asSymbol in destination
	MSG_CLASS,     // len + name  -> getClass in destination
	MSG_OBJECT,    // class-name + hash + indexedSize + (bytes | fields) + [float]
	MSG_BACKREF,   // id of an already-seen object
};

// A boxed Float keeps its double in one unscanned payload word (no pointer/
// indexed slots), so it needs its bits serialized explicitly.
static _Bool isFloatShape(InstanceShape shape)
{
	return shape.payloadSize == 1 && shape.varsSize == 0 && !shape.isIndexed;
}

// ---------------------------------------------------------------------------
// growable write buffer
typedef struct { uint8_t *data; size_t size, cap; _Bool error; } MsgBuf;

static void bufPut(MsgBuf *b, const void *p, size_t n)
{
	if (b->size + n > b->cap) {
		b->cap = (b->size + n) * 2 + 64;
		b->data = realloc(b->data, b->cap);
	}
	memcpy(b->data + b->size, p, n);
	b->size += n;
}
static void bufU8(MsgBuf *b, uint8_t v) { bufPut(b, &v, 1); }
static void bufU32(MsgBuf *b, uint32_t v) { bufPut(b, &v, sizeof(v)); }
static void bufI64(MsgBuf *b, int64_t v) { bufPut(b, &v, sizeof(v)); }

// object -> sequential id map (open addressed; power-of-two capacity)
typedef struct { RawObject **keys; uint32_t *ids; size_t cap, count; } IdMap;

static void idmapInit(IdMap *m)
{
	m->cap = 1024; m->count = 0;
	m->keys = calloc(m->cap, sizeof(RawObject *));
	m->ids = calloc(m->cap, sizeof(uint32_t));
}
static void idmapFree(IdMap *m) { free(m->keys); free(m->ids); }

static void idmapGrow(IdMap *m)
{
	size_t oldCap = m->cap;
	RawObject **oldKeys = m->keys;
	uint32_t *oldIds = m->ids;
	m->cap *= 2;
	m->keys = calloc(m->cap, sizeof(RawObject *));
	m->ids = calloc(m->cap, sizeof(uint32_t));
	for (size_t i = 0; i < oldCap; i++) {
		if (oldKeys[i] != NULL) {
			size_t h = ((uintptr_t) oldKeys[i] >> 4) & (m->cap - 1);
			while (m->keys[h] != NULL) h = (h + 1) & (m->cap - 1);
			m->keys[h] = oldKeys[i];
			m->ids[h] = oldIds[i];
		}
	}
	free(oldKeys); free(oldIds);
}

// If key is present, set *outId and return 1. Else insert with `newId`, return 0.
static _Bool idmapGetOrPut(IdMap *m, RawObject *key, uint32_t newId, uint32_t *outId)
{
	if (m->count * 4 >= m->cap * 3) idmapGrow(m);
	size_t h = ((uintptr_t) key >> 4) & (m->cap - 1);
	while (m->keys[h] != NULL) {
		if (m->keys[h] == key) { *outId = m->ids[h]; return 1; }
		h = (h + 1) & (m->cap - 1);
	}
	m->keys[h] = key; m->ids[h] = newId; m->count++;
	*outId = newId;
	return 0;
}

// ---------------------------------------------------------------------------
// serialization (reads the current heap; allocates nothing -> GC-stable)

typedef struct { MsgBuf buf; IdMap map; uint32_t nextId; } Writer;

static _Bool isClassObject(RawObject *o)
{
	// `o` is a class iff its class (metaclass) is an instance of Metaclass.
	return (RawObject *) ((RawClass *) o->class)->class == (RawObject *) Handles.MetaClass->raw;
}

static _Bool isRejectedClass(RawObject *cls)
{
	return cls == (RawObject *) Handles.Block->raw || cls == (RawObject *) Handles.BlockContext->raw
		|| cls == (RawObject *) Handles.MethodContext->raw || cls == (RawObject *) Handles.CompiledMethod->raw
		|| cls == (RawObject *) Handles.CompiledBlock->raw || cls == (RawObject *) Handles.ExceptionHandler->raw;
}

static void writeName(Writer *w, RawObject *stringOrSymbol)
{
	uint32_t len = (uint32_t) rawObjectSize(stringOrSymbol);
	bufU32(&w->buf, len);
	bufPut(&w->buf, getRawObjectIndexedVars(stringOrSymbol), len);
}

static void writeValue(Writer *w, Value v);

static void writeObjectRef(Writer *w, RawObject *o)
{
	if (w->buf.error) return;
	if (o == Handles.nil->raw)   { bufU8(&w->buf, MSG_NIL); return; }
	if (o == Handles.true->raw)  { bufU8(&w->buf, MSG_TRUE); return; }
	if (o == Handles.false->raw) { bufU8(&w->buf, MSG_FALSE); return; }

	uint32_t id;
	if (idmapGetOrPut(&w->map, o, w->nextId, &id)) {
		bufU8(&w->buf, MSG_BACKREF);
		bufU32(&w->buf, id);
		return;
	}
	w->nextId++; // consumed the id we just handed out

	RawObject *cls = (RawObject *) o->class;
	if (cls == (RawObject *) Handles.Symbol->raw) {
		bufU8(&w->buf, MSG_SYMBOL);
		writeName(w, o);
		return;
	}
	if (isClassObject(o)) {
		bufU8(&w->buf, MSG_CLASS);
		writeName(w, (RawObject *) asObject(((RawClass *) o)->name));
		return;
	}
	if (isRejectedClass(cls)) { w->buf.error = 1; return; }

	InstanceShape shape = ((RawClass *) cls)->instanceShape;
	bufU8(&w->buf, MSG_OBJECT);
	writeName(w, (RawObject *) asObject(((RawClass *) cls)->name)); // class by name
	bufI64(&w->buf, (int64_t) o->hash);                            // preserve identity hash
	size_t indexed = shape.isIndexed ? rawObjectSize(o) : 0;
	bufU32(&w->buf, (uint32_t) indexed);
	if (shape.isIndexed && shape.isBytes) {
		bufPut(&w->buf, getRawObjectIndexedVars(o), indexed);
	} else {
		size_t nfields = shape.varsSize + (shape.isIndexed ? indexed : 0);
		Value *vars = getRawObjectVars(o);
		for (size_t i = 0; i < nfields; i++) writeValue(w, vars[i]);
	}
	if (isFloatShape(shape)) bufI64(&w->buf, *(int64_t *) &((RawFloat *) o)->value);
}

static void writeValue(Writer *w, Value v)
{
	if (w->buf.error) return;
	if (!valueTypeOf(v, VALUE_POINTER)) {
		bufU8(&w->buf, MSG_INT);
		bufI64(&w->buf, (int64_t) v);
		return;
	}
	writeObjectRef(w, asObject(v));
}

uint8_t *messageSerialize(Value root, size_t *outSize)
{
	Writer w = { .buf = { 0 }, .nextId = 0 };
	idmapInit(&w.map);
	writeValue(&w, root); // handles both immediate roots (e.g. a bare integer) and objects
	idmapFree(&w.map);
	if (w.buf.error) {
		free(w.buf.data);
		*outSize = 0;
		return NULL;
	}
	*outSize = w.buf.size;
	return w.buf.data;
}

// ---------------------------------------------------------------------------
// deserialization (allocates into CurrentThread's heap; GC-safe via persistent
// handles for every materialized object — parents are re-fetched from handles
// before each field store, since allocations may move objects).

typedef struct { const uint8_t *data; size_t pos, size; _Bool error; } Cursor;

static uint8_t curU8(Cursor *c) { if (c->pos + 1 > c->size) { c->error = 1; return 0; } return c->data[c->pos++]; }
static uint32_t curU32(Cursor *c) { uint32_t v = 0; if (c->pos + 4 > c->size) { c->error = 1; return 0; } memcpy(&v, c->data + c->pos, 4); c->pos += 4; return v; }
static int64_t curI64(Cursor *c) { int64_t v = 0; if (c->pos + 8 > c->size) { c->error = 1; return 0; } memcpy(&v, c->data + c->pos, 8); c->pos += 8; return v; }
static const uint8_t *curBytes(Cursor *c, size_t n) { if (c->pos + n > c->size) { c->error = 1; return NULL; } const uint8_t *p = c->data + c->pos; c->pos += n; return p; }

typedef struct { Object **handles; size_t count, cap; } Registry;

static Object *regNew(Registry *r, RawObject *raw) // persistent-handle + record by id
{
	if (r->count == r->cap) {
		r->cap = r->cap ? r->cap * 2 : 256;
		r->handles = realloc(r->handles, r->cap * sizeof(Object *));
	}
	Object *h = handle(raw); // persistent handle (GC-updated, freeable)
	r->handles[r->count++] = h;
	return h;
}

static Value readValue(Cursor *c, Registry *reg);

static Value readValue(Cursor *c, Registry *reg)
{
	uint8_t tag = curU8(c);
	switch (tag) {
	case MSG_INT:   return (Value) curI64(c);
	case MSG_NIL:   return tagPtr(Handles.nil->raw);
	case MSG_TRUE:  return tagPtr(Handles.true->raw);
	case MSG_FALSE: return tagPtr(Handles.false->raw);
	case MSG_BACKREF: {
		uint32_t id = curU32(c);
		if (id >= reg->count) { c->error = 1; return tagPtr(Handles.nil->raw); }
		return tagPtr(reg->handles[id]->raw);
	}
	case MSG_SYMBOL: {
		uint32_t len = curU32(c);
		const uint8_t *bytes = curBytes(c, len);
		if (c->error) return tagPtr(Handles.nil->raw);
		String *str = (String *) newObject(Handles.String, len);
		memcpy(getRawObjectIndexedVars((RawObject *) str->raw), bytes, len);
		String *sym = asSymbol(str);
		regNew(reg, (RawObject *) sym->raw);
		return tagPtr(sym->raw);
	}
	case MSG_CLASS: {
		uint32_t len = curU32(c);
		const uint8_t *bytes = curBytes(c, len);
		if (c->error) return tagPtr(Handles.nil->raw);
		char name[512];
		if (len >= sizeof(name)) { c->error = 1; return tagPtr(Handles.nil->raw); }
		memcpy(name, bytes, len); name[len] = 0;
		Class *cls = getClass(name);
		RawObject *raw = cls != NULL ? (RawObject *) cls->raw : Handles.nil->raw;
		regNew(reg, raw);
		return tagPtr(raw);
	}
	case MSG_OBJECT: {
		uint32_t nameLen = curU32(c);
		const uint8_t *nameBytes = curBytes(c, nameLen);
		if (c->error) return tagPtr(Handles.nil->raw);
		char name[512];
		if (nameLen >= sizeof(name)) { c->error = 1; return tagPtr(Handles.nil->raw); }
		memcpy(name, nameBytes, nameLen); name[nameLen] = 0;
		Class *cls = getClass(name);
		if (cls == NULL) { c->error = 1; return tagPtr(Handles.nil->raw); }
		int64_t hash = curI64(c);
		uint32_t indexed = curU32(c);

		RawObject *obj = allocateObject(&CurrentThread.heap, cls->raw, indexed);
		obj->hash = (Value) hash;
		size_t myId = reg->count;
		regNew(reg, obj); // record BEFORE reading fields, so cycles resolve

		InstanceShape shape = cls->raw->instanceShape;
		if (shape.isIndexed && shape.isBytes) {
			const uint8_t *bytes = curBytes(c, indexed);
			if (!c->error) memcpy(getRawObjectIndexedVars(reg->handles[myId]->raw), bytes, indexed);
		} else {
			size_t nfields = shape.varsSize + (shape.isIndexed ? indexed : 0);
			for (size_t i = 0; i < nfields; i++) {
				Value fv = readValue(c, reg); // may allocate + move objects
				if (c->error) break;
				RawObject *parent = reg->handles[myId]->raw; // re-fetch: GC may have moved it
				Value *field = &getRawObjectVars(parent)[i];
				if (valueTypeOf(fv, VALUE_POINTER)) {
					rawObjectStorePtr(parent, field, asObject(fv)); // write barrier
				} else {
					*field = fv;
				}
			}
		}
		if (isFloatShape(shape)) {
			int64_t bits = curI64(c);
			((RawFloat *) reg->handles[myId]->raw)->value = *(double *) &bits;
		}
		return tagPtr(reg->handles[myId]->raw);
	}
	default:
		c->error = 1;
		return tagPtr(Handles.nil->raw);
	}
}

_Bool messageDeserialize(const uint8_t *bytes, size_t size, Value *out)
{
	Cursor c = { .data = bytes, .pos = 0, .size = size, .error = 0 };
	Registry reg = { 0 };
	Value root = readValue(&c, &reg);
	// Re-root the whole graph via its root in the CALLER's handle scope, then drop
	// the per-object persistent handles (no allocation happens between, so nothing
	// moves). The graph stays alive via the caller's scope until it uses/roots the
	// returned value (an immediate root, e.g. a SmallInteger, needs no rooting).
	Object *rooted = valueTypeOf(root, VALUE_POINTER) ? scopeHandle(asObject(root)) : NULL;
	for (size_t i = 0; i < reg.count; i++) freeHandle(reg.handles[i]);
	free(reg.handles);
	if (c.error) return 0;
	*out = rooted != NULL ? getTaggedPtr(rooted) : root;
	return 1;
}

// ---------------------------------------------------------------------------
// self-test: round-trip a graph built from Smalltalk source through the current
// heap and check structural equality (same class, same fields/bytes, recursively).

static _Bool structEqual(RawObject *a, RawObject *b, int depth)
{
	if (a == b) return 1;
	if (depth > 64) return 1; // cycle guard
	if (a->class != b->class) return 0;
	InstanceShape shape = ((RawClass *) a->class)->instanceShape;
	size_t sa = shape.isIndexed ? rawObjectSize(a) : 0;
	size_t sb = shape.isIndexed ? rawObjectSize(b) : 0;
	if (sa != sb) return 0;
	if (shape.isIndexed && shape.isBytes) {
		return memcmp(getRawObjectIndexedVars(a), getRawObjectIndexedVars(b), sa) == 0;
	}
	size_t n = shape.varsSize + (shape.isIndexed ? sa : 0);
	Value *va = getRawObjectVars(a);
	Value *vb = getRawObjectVars(b);
	for (size_t i = 0; i < n; i++) {
		if (valueTypeOf(va[i], VALUE_POINTER) != valueTypeOf(vb[i], VALUE_POINTER)) return 0;
		if (valueTypeOf(va[i], VALUE_POINTER)) {
			if (!structEqual(asObject(va[i]), asObject(vb[i]), depth + 1)) return 0;
		} else if (va[i] != vb[i]) {
			return 0;
		}
	}
	return 1;
}

int messageSelfTest(void)
{
	HandleScope scope;
	openHandleScope(&scope);
	const char *cases[] = {
		"'hello world'",
		"#(1 2 3 4 5)",
		"#(1 'two' $3 #four 5)",
		"3.14159",
		"| d | d := Dictionary new. d at: #one put: 1. d at: #two put: #(2 3 4). d at: 'name' put: 'bob'. d",
		"(1 to: 50) asArray",
	};
	int failures = 0;
	for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		Value origV = evalObject((char *) cases[k]);
		if (!valueTypeOf(origV, VALUE_POINTER)) { fprintf(stderr, "  case %zu: eval did not yield an object\n", k); failures++; continue; }
		Object *orig = scopeHandle(asObject(origV));
		size_t size = 0;
		uint8_t *bytes = messageSerialize(getTaggedPtr(orig), &size);
		if (bytes == NULL) { fprintf(stderr, "  case %zu: serialize rejected\n", k); failures++; continue; }
		Value copyV;
		_Bool ok = messageDeserialize(bytes, size, &copyV);
		free(bytes);
		if (!ok || !valueTypeOf(copyV, VALUE_POINTER)) { fprintf(stderr, "  case %zu: deserialize failed\n", k); failures++; continue; }
		Object *copy = scopeHandle(asObject(copyV));
		_Bool eq = structEqual(orig->raw, copy->raw, 0);
		_Bool distinct = orig->raw != copy->raw;
		fprintf(stderr, "  case %zu: bytes=%zu equal=%d distinct=%d\n", k, size, eq, distinct);
		if (!eq || !distinct) failures++;
	}

	closeHandleScope(&scope, NULL);
	fprintf(stderr, "message self-test: %d failures\n", failures);
	return failures;
}
