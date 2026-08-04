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
// LAYOUT. FORMAT_INDEXED_POINTERS with two named slots, so the whole object is
// tagged and the collector needs no special case:
//
//     body word 0    capture count      (the indexed formats' element count)
//     body word 1    method             the CompiledMethod holding this block's
//                                       code unit and its compiled code
//     body word 2    home token         which activation a `^` inside this block
//                                       returns from, as a tagged SmallInteger
//     body word 3+i  captured[i]
//
// The token is TAGGED rather than raw so that the object stays uniformly tagged
// and the collector still needs no case for it. It names an activation and not
// an address: a token is minted per activation and never reused, so a block that
// outlives its home finds nothing to return to, rather than finding whatever
// frame now sits where its home used to be (ADR 0008).
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
	Value homeToken;
	Value captured[];
} RawClosure;
OBJECT_HANDLE(Closure);

#define CLOSURE_NAMED_SLOTS 2
#define CLOSURE_SHAPE \
	((InstanceShape) DEFINE_SHAPE(FORMAT_INDEXED_POINTERS, 0, 0, CLOSURE_NAMED_SLOTS))
// Field index of captured[i], for generated code: body word 0 is the count,
// word 1 is the method and word 2 is the home token.
#define CLOSURE_CAPTURE_FIELD(i) ((uint16_t) (3 + (i)))
#define CLOSURE_METHOD_FIELD 1
#define CLOSURE_HOME_FIELD 2

// The most a closure may capture, and the tighter of two ceilings wins.
//
// The CLOSURE instruction counts captures in one byte, which allows 255. The
// real limit is smaller and comes from the object model: a closure is an INDEXED
// object that ALSO has a named slot, and the collector sizes such an object from
// the header alone (memory/ObjectWalk.h), so it has to fit the header's size
// field. Worst case a closure occupies 4 + captures words after alignment.
//
// The front end reports going past this as a clean compile error, which is what
// this bytecode does with every ceiling.
//
// Worst case a closure occupies 5 + captures words: header, count, method, home
// token, the captures, and one word of alignment padding.
#define CLOSURE_MAX_CAPTURES 248
_Static_assert(CLOSURE_MAX_CAPTURES + 5 < SIZE_WORDS_BIG,
	"a closure has to stay sizeable from its header alone");

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

Closure *newClosure(Object *method, uint16_t captureCount, uint64_t homeToken);
Cell *newCell(Value value);

#endif
