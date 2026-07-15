// C runtime for smalltalk/Json.st — a strict RFC 8259 parser and a core-type
// encoder. The design contract with the GC (see Dictionary.c's growDictionary
// for the house pattern):
//
//   * The input String is COPIED to a malloc'd buffer up front: every Smalltalk
//     allocation may move young objects, so no cursor may point into the heap.
//   * Every heap object created here is held via scopeHandle (never a raw
//     pointer across an allocation), and container-filling loops open one
//     nested HandleScope per element so no scope ever nears the 256-slot cap.
//   * Pointer stores into heap objects go through the write barrier
//     (stringDictAtPutObject / objectStorePtr).
//   * Anything outside the fast common case returns 0 — the <primitive:>
//     trampoline then falls through to the Smalltalk fallback, which is the
//     validating reference implementation (precise errors, LargeInteger).

#include "runtime/Json.h"
#include "core/Handle.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "core/Smalltalk.h"
#include "core/Thread.h"
#include "core/Assert.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct {
	const char *p;       // cursor into the malloc'd copy (NUL-terminated)
	const char *end;
	char *scratch;       // reusable decode buffer for strings with escapes
	size_t scratchCap;
} JsonParser;

// One parsed node. Tagged SmallIntegers are truly immediate (GC-immune);
// everything else — including true/false/nil — travels as a handle so a
// scavenge between creation and the container store cannot invalidate it.
typedef struct {
	_Bool isImmediate;
	Value immediate;
	Object *object;
} JsonValue;

static _Bool parseValue(JsonParser *j, int depth, JsonValue *out);

static void skipWs(JsonParser *j)
{
	const char *p = j->p;
	while (p < j->end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
		p++;
	}
	j->p = p;
}

static _Bool scratchEnsure(JsonParser *j, size_t need)
{
	if (need <= j->scratchCap) {
		return 1;
	}
	size_t cap = j->scratchCap ? j->scratchCap : 64;
	while (cap < need) {
		cap *= 2;
	}
	char *grown = realloc(j->scratch, cap);
	if (grown == NULL) {
		return 0;
	}
	j->scratch = grown;
	j->scratchCap = cap;
	return 1;
}

static _Bool parseLiteral(JsonParser *j, const char *word, size_t size)
{
	if ((size_t) (j->end - j->p) < size || memcmp(j->p, word, size) != 0) {
		return 0;
	}
	j->p += size;
	return 1;
}

static _Bool readHex4(const char *p, const char *end, unsigned *out)
{
	unsigned code = 0;
	if (end - p < 4) {
		return 0;
	}
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		unsigned digit;
		if (c >= '0' && c <= '9') {
			digit = (unsigned) (c - '0');
		} else if (c >= 'a' && c <= 'f') {
			digit = (unsigned) (c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			digit = (unsigned) (c - 'A' + 10);
		} else {
			return 0;
		}
		code = (code << 4) | digit;
	}
	*out = code;
	return 1;
}

static size_t utf8Encode(unsigned cp, char *dst)
{
	if (cp < 0x80) {
		dst[0] = (char) cp;
		return 1;
	}
	if (cp < 0x800) {
		dst[0] = (char) (0xC0 | (cp >> 6));
		dst[1] = (char) (0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		dst[0] = (char) (0xE0 | (cp >> 12));
		dst[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
		dst[2] = (char) (0x80 | (cp & 0x3F));
		return 3;
	}
	dst[0] = (char) (0xF0 | (cp >> 18));
	dst[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
	dst[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
	dst[3] = (char) (0x80 | (cp & 0x3F));
	return 4;
}

// Cursor sits on the opening quote. Decodes into the scratch buffer (bulk
// memcpy for escape-free runs), then materializes ONE String at the end — the
// single allocation, after which no parser state points into the heap.
static _Bool parseString(JsonParser *j, String **out)
{
	const char *p = j->p + 1;
	size_t len = 0;

	for (;;) {
		const char *run = p;
		while (p < j->end && (unsigned char) *p >= 0x20 && *p != '"' && *p != '\\') {
			p++;
		}
		if (p > run) {
			if (!scratchEnsure(j, len + (size_t) (p - run))) {
				return 0;
			}
			memcpy(j->scratch + len, run, (size_t) (p - run));
			len += (size_t) (p - run);
		}
		if (p >= j->end) {
			return 0; // unterminated string
		}
		if (*p == '"') {
			p++;
			break;
		}
		if ((unsigned char) *p < 0x20) {
			return 0; // raw control character
		}
		p++; // consume the backslash
		if (p >= j->end) {
			return 0;
		}
		char c = *p++;
		char simple;
		switch (c) {
		case '"':  simple = '"';  break;
		case '\\': simple = '\\'; break;
		case '/':  simple = '/';  break;
		case 'n':  simple = '\n'; break;
		case 't':  simple = '\t'; break;
		case 'r':  simple = '\r'; break;
		case 'b':  simple = '\b'; break;
		case 'f':  simple = '\f'; break;
		case 'u': {
			unsigned cp;
			if (!readHex4(p, j->end, &cp)) {
				return 0;
			}
			p += 4;
			if (cp >= 0xD800 && cp <= 0xDBFF) {
				unsigned lo;
				if (j->end - p < 6 || p[0] != '\\' || p[1] != 'u'
					|| !readHex4(p + 2, j->end, &lo)
					|| lo < 0xDC00 || lo > 0xDFFF) {
					return 0; // high surrogate demands a low surrogate
				}
				p += 6;
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
			} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
				return 0; // lone low surrogate
			}
			if (!scratchEnsure(j, len + 4)) {
				return 0;
			}
			len += utf8Encode(cp, j->scratch + len);
			continue;
		}
		default:
			return 0; // invalid escape
		}
		if (!scratchEnsure(j, len + 1)) {
			return 0;
		}
		j->scratch[len++] = simple;
	}

	j->p = p;
	String *s = newString(len);
	memcpy(s->raw->contents, j->scratch, len);
	*out = s;
	return 1;
}

// Strict grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
// Integers that fit a SmallInteger become immediates; anything fractional or
// exponential goes through strtod (the buffer is NUL-terminated, and the
// token was validated, so strtod consumes exactly the token). Integers beyond
// the SmallInteger range return 0 -> the Smalltalk fallback promotes them to
// LargeInteger.
static _Bool parseNumber(JsonParser *j, JsonValue *out)
{
	const char *start = j->p;
	const char *p = j->p;
	_Bool isFloat = 0;
	_Bool negative = 0;

	if (*p == '-') {
		negative = 1;
		p++;
	}
	if (!(*p >= '0' && *p <= '9')) {
		return 0;
	}
	if (*p == '0') {
		p++;
		if (*p >= '0' && *p <= '9') {
			return 0; // leading zeros are not allowed
		}
	} else {
		while (*p >= '0' && *p <= '9') {
			p++;
		}
	}
	if (*p == '.') {
		isFloat = 1;
		p++;
		if (!(*p >= '0' && *p <= '9')) {
			return 0;
		}
		while (*p >= '0' && *p <= '9') {
			p++;
		}
	}
	if (*p == 'e' || *p == 'E') {
		isFloat = 1;
		p++;
		if (*p == '+' || *p == '-') {
			p++;
		}
		if (!(*p >= '0' && *p <= '9')) {
			return 0;
		}
		while (*p >= '0' && *p <= '9') {
			p++;
		}
	}

	if (isFloat) {
		double d = strtod(start, NULL);
		j->p = p;
		Float *f = newFloat(d);
		out->isImmediate = 0;
		out->object = (Object *) f;
		return 1;
	}

	// SmallInteger fast path with an exact overflow bail (tagInt range is
	// +-(2^62 - 1)).
	const unsigned long long limit = 0x3FFFFFFFFFFFFFFFULL;
	unsigned long long acc = 0;
	const char *q = start + (negative ? 1 : 0);
	for (; q < p; q++) {
		unsigned digit = (unsigned) (*q - '0');
		if (acc > (limit - digit) / 10) {
			return 0; // beyond SmallInteger -> Smalltalk promotes to LargeInteger
		}
		acc = acc * 10 + digit;
	}
	j->p = p;
	out->isImmediate = 1;
	out->immediate = tagInt(negative ? -(intptr_t) acc : (intptr_t) acc);
	return 1;
}

// ordCollAddObject with GEOMETRIC growth (the Collection.c version grows +8 a
// time, which is O(n^2) on big JSON arrays). The plain memcpy of the old
// contents is safe even when the new Array lands in old space: allocateObject
// remembers every old-space birth, so the next scavenge scans the young
// referents it received.
static void jsonOrdCollAppend(OrderedCollection *collection, JsonValue *v)
{
	Array *contents = scopeHandle(ordCollGetContents(collection));
	size_t size = ordCollSize(collection);

	if (size == (size_t) contents->raw->size) {
		size_t grown = size < 8 ? 8 : size * 2;
		Array *newContents = newObject(Handles.Array, grown);
		memcpy(newContents->raw->vars, contents->raw->vars, size * sizeof(Value));
		objectStorePtr((Object *) collection, &collection->raw->contents, (Object *) newContents);
		contents = newContents;
	}
	intptr_t lastIndex = ordCollGetLastIndex(collection);
	if (v->isImmediate) {
		contents->raw->vars[lastIndex] = v->immediate;
	} else {
		objectStorePtr((Object *) contents, &contents->raw->vars[lastIndex], v->object);
	}
	collection->raw->lastIndex = tagInt(lastIndex + 1);
}

static _Bool parseObject(JsonParser *j, int depth, JsonValue *out)
{
	Dictionary *dict = newDictionary(8);

	j->p++; // consume '{'
	skipWs(j);
	if (*j->p == '}') {
		j->p++;
		out->isImmediate = 0;
		out->object = (Object *) dict;
		return 1;
	}

	for (;;) {
		// One nested scope per entry: the key, the value and the internals of
		// dictAtPut stay bounded no matter how many entries the object has.
		HandleScope scope;
		openHandleScope(&scope);

		String *key;
		JsonValue val;
		skipWs(j);
		if (*j->p != '"' || !parseString(j, &key)) {
			closeHandleScope(&scope, NULL);
			return 0;
		}
		skipWs(j);
		if (*j->p != ':') {
			closeHandleScope(&scope, NULL);
			return 0;
		}
		j->p++;
		if (!parseValue(j, depth + 1, &val)) {
			closeHandleScope(&scope, NULL);
			return 0;
		}
		if (val.isImmediate) {
			// Value-variant is safe ONLY for immediates: it allocates the
			// Association after reading the value.
			stringDictAtPut(dict, key, val.immediate);
		} else {
			stringDictAtPutObject(dict, key, val.object);
		}
		skipWs(j);
		char c = j->p < j->end ? *j->p : 0;
		closeHandleScope(&scope, NULL);

		if (c == '}') {
			j->p++;
			break;
		}
		if (c != ',') {
			return 0;
		}
		j->p++;
	}

	out->isImmediate = 0;
	out->object = (Object *) dict;
	return 1;
}

static _Bool parseArray(JsonParser *j, int depth, JsonValue *out)
{
	OrderedCollection *array = newOrdColl(8);

	j->p++; // consume '['
	skipWs(j);
	if (*j->p == ']') {
		j->p++;
		out->isImmediate = 0;
		out->object = (Object *) array;
		return 1;
	}

	for (;;) {
		HandleScope scope;
		openHandleScope(&scope);

		JsonValue val;
		if (!parseValue(j, depth + 1, &val)) {
			closeHandleScope(&scope, NULL);
			return 0;
		}
		jsonOrdCollAppend(array, &val);
		skipWs(j);
		char c = j->p < j->end ? *j->p : 0;
		closeHandleScope(&scope, NULL);

		if (c == ']') {
			j->p++;
			break;
		}
		if (c != ',') {
			return 0;
		}
		j->p++;
	}

	out->isImmediate = 0;
	out->object = (Object *) array;
	return 1;
}

static _Bool parseValue(JsonParser *j, int depth, JsonValue *out)
{
	if (depth > JSON_MAX_DEPTH) {
		return 0;
	}
	skipWs(j);
	if (j->p >= j->end) {
		return 0;
	}
	switch (*j->p) {
	case '{':
		return parseObject(j, depth, out);
	case '[':
		return parseArray(j, depth, out);
	case '"': {
		String *s;
		if (!parseString(j, &s)) {
			return 0;
		}
		out->isImmediate = 0;
		out->object = (Object *) s;
		return 1;
	}
	case 't':
		if (!parseLiteral(j, "true", 4)) {
			return 0;
		}
		out->isImmediate = 0;
		out->object = Handles.true;
		return 1;
	case 'f':
		if (!parseLiteral(j, "false", 5)) {
			return 0;
		}
		out->isImmediate = 0;
		out->object = Handles.false;
		return 1;
	case 'n':
		if (!parseLiteral(j, "null", 4)) {
			return 0;
		}
		out->isImmediate = 0;
		out->object = Handles.nil;
		return 1;
	default:
		if (*j->p == '-' || (*j->p >= '0' && *j->p <= '9')) {
			return parseNumber(j, out);
		}
		return 0;
	}
}

_Bool jsonParse(String *input, Value *result)
{
	size_t size = (size_t) input->raw->size;
	char *copy = malloc(size + 1);
	if (copy == NULL) {
		return 0;
	}
	memcpy(copy, input->raw->contents, size);
	copy[size] = 0;

	JsonParser j = { .p = copy, .end = copy + size, .scratch = NULL, .scratchCap = 0 };
	JsonValue v;
	_Bool ok = parseValue(&j, 0, &v);
	if (ok) {
		skipWs(&j);
		ok = (j.p == j.end); // trailing garbage is a syntax error
	}
	free(j.scratch);
	free(copy);
	if (!ok) {
		return 0;
	}
	*result = v.isImmediate ? v.immediate : getTaggedPtr(v.object);
	return 1;
}


// ---- encoder ---------------------------------------------------------------
// Walks a graph of CORE JSON types only, by raw pointers, into a malloc'd
// byte buffer: there is NO Smalltalk allocation until the single newString at
// the very end, so nothing can move under the walk. Any non-core class, a
// NaN/Infinity Float or over-depth nesting returns 0 and the whole encode
// falls back to the reflective Smalltalk path (which re-enters this fast path
// for core subtrees).

typedef struct {
	char *buf;
	size_t len;
	size_t cap;
} JsonBuf;

static _Bool bufEnsure(JsonBuf *b, size_t need)
{
	if (b->len + need <= b->cap) {
		return 1;
	}
	size_t cap = b->cap;
	while (cap < b->len + need) {
		cap *= 2;
	}
	char *grown = realloc(b->buf, cap);
	if (grown == NULL) {
		return 0;
	}
	b->buf = grown;
	b->cap = cap;
	return 1;
}

static _Bool bufWrite(JsonBuf *b, const char *src, size_t n)
{
	if (!bufEnsure(b, n)) {
		return 0;
	}
	memcpy(b->buf + b->len, src, n);
	b->len += n;
	return 1;
}

static _Bool bufByte(JsonBuf *b, char c)
{
	if (!bufEnsure(b, 1)) {
		return 0;
	}
	b->buf[b->len++] = c;
	return 1;
}

size_t jsonFormatDouble(double x, char *buf)
{
	if (x == 0.0) {
		strcpy(buf, signbit(x) ? "-0.0" : "0.0");
		return strlen(buf);
	}
	/* shortest number of significant digits that round-trips */
	int sig = 17;
	for (int s = 1; s <= 17; s++) {
		snprintf(buf, 64, "%.*e", s - 1, x);
		if (strtod(buf, NULL) == x) {
			sig = s;
			break;
		}
	}
	/* prefer plain decimal notation for human-friendly magnitudes */
	int exp10 = (int) floor(log10(fabs(x)));
	if (exp10 >= -4 && exp10 < 16) {
		int frac = sig - 1 - exp10;
		if (frac < 0) {
			frac = 0;
		}
		snprintf(buf, 64, "%.*f", frac, x);
		if (strtod(buf, NULL) != x) {
			snprintf(buf, 64, "%.*e", sig - 1, x);
		}
	} else {
		snprintf(buf, 64, "%.*e", sig - 1, x);
	}
	if (strpbrk(buf, ".eE") == NULL) {
		strcat(buf, ".0"); /* keep integral floats visibly Float on re-parse */
	}
	return strlen(buf);
}

// 0 = raw passthrough (UTF-8 bytes included); 'u' = \u00XX; else the escape char.
static const char jsonEscapeTable[256] = {
	['"'] = '"', ['\\'] = '\\', ['\n'] = 'n', ['\r'] = 'r', ['\t'] = 't',
	['\b'] = 'b', ['\f'] = 'f',
	[0] = 'u', [1] = 'u', [2] = 'u', [3] = 'u', [4] = 'u', [5] = 'u',
	[6] = 'u', [7] = 'u', [11] = 'u', [14] = 'u', [15] = 'u', [16] = 'u',
	[17] = 'u', [18] = 'u', [19] = 'u', [20] = 'u', [21] = 'u', [22] = 'u',
	[23] = 'u', [24] = 'u', [25] = 'u', [26] = 'u', [27] = 'u', [28] = 'u',
	[29] = 'u', [30] = 'u', [31] = 'u',
};

static _Bool encodeStringBody(JsonBuf *b, RawString *s)
{
	static const char hex[] = "0123456789ABCDEF";
	const char *p = s->contents;
	const char *end = p + (size_t) s->size;

	if (!bufByte(b, '"')) {
		return 0;
	}
	while (p < end) {
		const char *run = p;
		while (p < end && jsonEscapeTable[(unsigned char) *p] == 0) {
			p++;
		}
		if (p > run && !bufWrite(b, run, (size_t) (p - run))) {
			return 0;
		}
		if (p >= end) {
			break;
		}
		char esc = jsonEscapeTable[(unsigned char) *p];
		if (esc == 'u') {
			char seq[6] = { '\\', 'u', '0', '0',
				hex[((unsigned char) *p) >> 4], hex[((unsigned char) *p) & 15] };
			if (!bufWrite(b, seq, 6)) {
				return 0;
			}
		} else {
			char seq[2] = { '\\', esc };
			if (!bufWrite(b, seq, 2)) {
				return 0;
			}
		}
		p++;
	}
	return bufByte(b, '"');
}

static _Bool encodeValue(JsonBuf *b, Value v, int depth);

static _Bool encodeDictBody(JsonBuf *b, RawDictionary *dict, int depth)
{
	RawArray *contents = (RawArray *) asObject(dict->contents);
	_Bool first = 1;

	if (!bufByte(b, '{')) {
		return 0;
	}
	for (size_t i = 0; i < (size_t) contents->size; i++) {
		Value slot = contents->vars[i];
		if (!valueTypeOf(slot, VALUE_POINTER) || asObject(slot) == Handles.nil->raw) {
			continue;
		}
		RawAssociation *assoc = (RawAssociation *) asObject(slot);
		if (!valueTypeOf(assoc->key, VALUE_POINTER)) {
			return 0; // non-string key: Smalltalk fallback prints it
		}
		RawObject *key = asObject(assoc->key);
		if (key->class != Handles.String->raw && key->class != Handles.Symbol->raw) {
			return 0;
		}
		if (!first && !bufByte(b, ',')) {
			return 0;
		}
		first = 0;
		if (!encodeStringBody(b, (RawString *) key)) {
			return 0;
		}
		if (!bufByte(b, ':')) {
			return 0;
		}
		if (!encodeValue(b, assoc->value, depth + 1)) {
			return 0;
		}
	}
	return bufByte(b, '}');
}

static _Bool encodeSpan(JsonBuf *b, Value *vars, size_t count, int depth)
{
	if (!bufByte(b, '[')) {
		return 0;
	}
	for (size_t i = 0; i < count; i++) {
		if (i > 0 && !bufByte(b, ',')) {
			return 0;
		}
		if (!encodeValue(b, vars[i], depth + 1)) {
			return 0;
		}
	}
	return bufByte(b, ']');
}

static _Bool encodeValue(JsonBuf *b, Value v, int depth)
{
	if (depth > JSON_MAX_DEPTH) {
		return 0; // cycle or absurd nesting: the Smalltalk fallback raises JsonError
	}
	if (valueTypeOf(v, VALUE_INT)) {
		char num[32];
		int n = snprintf(num, sizeof(num), "%lld", (long long) asCInt(v));
		return bufWrite(b, num, (size_t) n);
	}
	if (!valueTypeOf(v, VALUE_POINTER)) {
		return 0; // Characters (and any future immediate) take the Smalltalk path
	}

	RawObject *object = asObject(v);
	if (object == Handles.nil->raw) {
		return bufWrite(b, "null", 4);
	}
	if (object == Handles.true->raw) {
		return bufWrite(b, "true", 4);
	}
	if (object == Handles.false->raw) {
		return bufWrite(b, "false", 5);
	}

	RawClass *class = object->class;
	if (class == Handles.String->raw || class == Handles.Symbol->raw) {
		return encodeStringBody(b, (RawString *) object);
	}
	if (class == Handles.Float->raw) {
		double x = rawFloatValue(object);
		if (isnan(x) || isinf(x)) {
			return 0; // no JSON representation: Smalltalk raises JsonError
		}
		char num[64];
		size_t n = jsonFormatDouble(x, num);
		return bufWrite(b, num, n);
	}
	if (class == Handles.Dictionary->raw) {
		return encodeDictBody(b, (RawDictionary *) object, depth);
	}
	if (class == Handles.OrderedCollection->raw) {
		RawOrderedCollection *coll = (RawOrderedCollection *) object;
		RawArray *contents = (RawArray *) asObject(coll->contents);
		intptr_t first = asCInt(coll->firstIndex);
		intptr_t last = asCInt(coll->lastIndex);
		return encodeSpan(b, contents->vars + (first - 1), (size_t) (last - first + 1), depth);
	}
	if (class == Handles.Array->raw) {
		RawArray *array = (RawArray *) object;
		return encodeSpan(b, array->vars, (size_t) array->size, depth);
	}
	return 0; // any other class: reflective Smalltalk fallback
}

_Bool jsonEncode(Value object, String **result)
{
	JsonBuf b = { .buf = malloc(512), .len = 0, .cap = 512 };
	if (b.buf == NULL) {
		return 0;
	}
	if (!encodeValue(&b, object, 0)) {
		free(b.buf);
		return 0;
	}
	String *s = newString(b.len);
	memcpy(s->raw->contents, b.buf, b.len);
	free(b.buf);
	*result = s;
	return 1;
}
