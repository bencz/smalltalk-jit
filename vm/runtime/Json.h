#ifndef JSON_H
#define JSON_H

#include "core/Object.h"
#include "runtime/String.h"

// Fast JSON runtime backing packages/Core/src/Json.st (which is also the validating
// fallback and the reference implementation — keep semantics in sync).
//
// jsonParse: strict RFC 8259 parse of `input` into a Smalltalk object graph
// (Dictionary / OrderedCollection / String / SmallInteger / Float / true /
// false / nil). Returns 1 and stores the tagged result in *result — when the
// result is a heap object it is protected by a handle in the CALLER's current
// HandleScope. Returns 0 on any syntax error, on integers beyond the
// SmallInteger range, or on nesting deeper than JSON_MAX_DEPTH; the Smalltalk
// fallback then re-parses for a precise JsonParseError (or a LargeInteger).
_Bool jsonParse(String *input, Value *result);

// jsonEncode: encode a graph made ONLY of core JSON types (exact classes:
// Dictionary, OrderedCollection, Array, String, Symbol, Float, plus
// SmallInteger / true / false / nil) into a JSON String (*result, handled in
// the caller's scope). Walks the graph with NO Smalltalk allocation, so raw
// pointers stay valid throughout. Returns 0 when the graph contains any other
// class, a NaN/Infinity Float, or nesting deeper than JSON_MAX_DEPTH — the
// Smalltalk fallback then walks that level reflectively.
_Bool jsonEncode(Value object, String **result);

// Shortest round-trip decimal form of a finite double (shared with
// floatAsStringPrimitive). Writes into buf (>= 64 bytes), returns the length.
size_t jsonFormatDouble(double x, char *buf);

#define JSON_MAX_DEPTH 128

#endif
