#ifndef MESSAGE_H
#define MESSAGE_H

#include "core/Object.h"
#include <stddef.h>
#include <stdint.h>

// Cross-isolate message (de)serialization. Because isolates are share-nothing,
// a message sent from one isolate's heap must be deep-COPIED into the target's
// heap: no object pointers cross the boundary. This serializes an object graph
// to a portable byte buffer and rebuilds it in the current isolate's heap,
// rebinding classes by NAME (the destination's same-named class) and interning
// symbols there. It copies DATA (SmallInts, Strings/Symbols, Arrays, Dictionaries,
// domain objects, Floats) and preserves identity hashes so hashed collections
// survive the copy. It REJECTS code (blocks, contexts, compiled methods) — those
// are not portable across heaps.

// Serialize the graph rooted at `root` (any Value — an immediate or a pointer)
// into a freshly malloc'd buffer. Sets *outSize and returns the buffer (caller
// frees). Returns NULL if the graph contains a non-portable object (block/
// context/compiled code).
uint8_t *messageSerialize(Value root, size_t *outSize);

// Rebuild the graph from `bytes` into CurrentThread's heap. On success sets *out
// to the tagged root Value and returns 1; returns 0 on malformed input. Error is
// reported out-of-band because a valid root can itself be 0 (tagInt(0) == 0). May
// allocate (and thus GC) — callers holding other live roots must have them in
// handles.
_Bool messageDeserialize(const uint8_t *bytes, size_t size, Value *out);

// C self-test: round-trip a graph built from Smalltalk source and compare.
int messageSelfTest(void);

#endif
