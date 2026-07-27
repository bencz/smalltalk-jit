#ifndef CLOSURE_H
#define CLOSURE_H

// Blocks, as FLAT closures with cells (ADR 0008).
//
// A block captures what it needs BY VALUE, into itself. There is no chain of
// contexts and no walking of levels: reaching a captured variable is one load
// at a known offset. The exception is a variable that is captured AND MUTATED,
// which gets a CELL on the heap; the closure then captures the cell's pointer
// by value like anything else, and reads and writes go through it.
//
// Why that shape rather than the old VM's contexts: escape analysis needs an
// EXPLICIT allocation to be able to erase it. A cell is one NEWCELL instruction
// with an SSA result and traceable uses, so the optimizer scalarises it exactly
// as it scalarises any other allocation, and the materialisation recipe for a
// failed guard comes from the same mechanism. A Context was opaque, so the
// activation's allocation never went away.
//
// And most blocks allocate no cell at all: a block that only READS outer
// variables copies them in, which is the common case in the targets of this
// project (the body of to:do:, the body of whileTrue:).
//
// LAYOUT. FORMAT_INDEXED_POINTERS with one named slot, so the whole object is
// tagged and the collector needs no special case:
//
//     body word 0    capture count      (the indexed formats' element count)
//     body word 1    method             the CompiledMethod holding this block's
//                                       code unit and its compiled code
//     body word 2+i  captured[i]
//
// The code is reached through a HEAP OBJECT rather than through raw C pointers,
// which is what keeps a closure's code unit reachable for as long as the closure
// is: the alternative is a raw word the collector cannot follow.

#include "core/Handle.h"
#include "core/Object.h"

typedef struct {
	OBJECT_HEADER;
	uint64_t captureCount;
	Value method;
	Value captured[];
} RawClosure;
OBJECT_HANDLE(Closure);

#define CLOSURE_NAMED_SLOTS 1
#define CLOSURE_SHAPE \
	((InstanceShape) DEFINE_SHAPE(FORMAT_INDEXED_POINTERS, 0, 0, CLOSURE_NAMED_SLOTS))
// Field index of captured[i], for generated code: body word 0 is the count and
// word 1 is the method.
#define CLOSURE_CAPTURE_FIELD(i) ((uint16_t) (2 + (i)))
#define CLOSURE_METHOD_FIELD 1

// A box for a variable that is captured and then assigned. One slot, so the
// value can change without the closures that captured it having to agree on
// anything but the box's address.
typedef struct {
	OBJECT_HEADER;
	Value value;
} RawCell;
OBJECT_HANDLE(Cell);

#define CELL_SHAPE ((InstanceShape) DEFINE_SHAPE(FORMAT_POINTERS, 0, 0, 1))
#define CELL_VALUE_FIELD 0

Closure *newClosure(Object *method, uint16_t captureCount);
Cell *newCell(Value value);

#endif
