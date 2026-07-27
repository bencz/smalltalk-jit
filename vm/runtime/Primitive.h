#ifndef PRIMITIVE_H
#define PRIMITIVE_H

// Primitives: what is on the far side of a send.
//
// A method may name a primitive. Compiled code attempts it immediately after
// the prologue, with the frame already built, and:
//
//   * on SUCCESS, returns the primitive's answer and never enters the bytecode;
//   * on FAILURE, falls through into the bytecode body, which is the general
//     case written in Smalltalk.
//
// That fall-through is the whole design and not a detail. It is what lets a
// primitive be narrow and fast without being wrong: `+` handles two
// SmallIntegers that do not overflow, and everything else -- overflow, a
// Fraction, a LargeInteger, a receiver that is none of the above -- is the
// method's own code. A primitive that cannot answer must fail; it must never
// guess.
//
// WHAT IS NOT HERE, AND WHY IT MATTERS (ADR 0006). Nothing in this file is
// reachable except through a SEND with an inline cache in front of it.
// Arithmetic, at:, at:put: and size are not resolved statically by the bytecode
// compiler, and they are not open-coded by the code generator. The old VM did
// open-code arithmetic at the call site, and when that fast path HIT it jumped
// over the inline cache entirely, so the profile at precisely the hottest sites
// described only the executions that MISSED. The profile there was not
// imprecise, it was inverted, and the optimizer that reads it is the whole
// point of the project.
//
// ALLOCATION takes one extra step, and skipping it is silent corruption.
//
// Allocating can trigger a collection, and a collection has to be able to walk
// the compiled frames underneath this primitive or it will evacuate objects that
// live methods are still holding in their registers. The frames are reachable,
// but only after the primitive ANCHORS them, which it does with
// PRIMITIVE_ALLOCATES below. Nothing enforces this by construction, so it is
// stated once here and every allocating primitive says so in its first line.
//
// The arithmetic primitives deliberately do NOT allocate and therefore do not
// anchor: a result that would need a heap box -- an overflowed sum, a double
// outside the SmallFloat64 window -- FAILS instead, and the method's own
// Smalltalk code builds the LargeInteger or the BoxedFloat64 where allocating
// is ordinary. Those failures are tested.

#include "core/Object.h"
#include "jit/Jit.h"
#include <stdint.h>

// The failure signal, in band.
//
// tagPtr(NULL): the pointer tag with address zero. No legal Value can equal it.
// tagInt shifts left by two, tagChar and tagFloat set their own low bits and
// shift, so none of the three can produce 1; and no object is ever allocated at
// address 0, so no tagged pointer can either. The cost is one compare against
// an immediate, with no out-parameter, no second return register and no memory
// traffic.
#define PRIMITIVE_FAILED ((Value) 1)

// `args` points at the RECEIVER's frame slot. The arguments follow at
// DESCENDING addresses, because consecutive bytecode registers are consecutive
// frame slots and slots grow down (jit/Jit.h). These two accessors are the only
// place that direction is written, so the frame contract has one encoding here
// exactly as it has one in the macro assembler.
typedef Value (*PrimitiveFunction)(Value *args, uint64_t argc);

// Anchor the calling method's frame for the duration of a primitive that
// allocates. `args` IS the address of frame slot 0, so the frame pointer follows
// from it, and the return address names the compiled method: both are already on
// hand, so this costs nothing in the generated code (jit/Jit.h).
//
// A MACRO and not a function, because __builtin_return_address(0) has to be
// taken in the primitive itself; inside a helper it would name the helper.
#define PRIMITIVE_ALLOCATES(args) \
	CompiledFrameGuard primitiveFrame_; \
	compiledFrameEnter(&primitiveFrame_, (args), 0, __builtin_return_address(0))
#define PRIMITIVE_DONE_ALLOCATING() compiledFrameLeave(&primitiveFrame_)


static inline Value primitiveReceiver(Value *args)
{
	return args[0];
}


static inline Value primitiveArgument(Value *args, uint64_t index)
{
	return args[-(intptr_t) (index + 1)];
}


// The primitives, by name.
//
// An ENUM and not a table position. The old VM numbered primitives by their
// order in a C array and baked that number into every CompiledMethod in the
// snapshot, so reordering the table silently rebound every method in the image.
// Here the snapshot is derived (docs/jit-v2/03-escopo-revisado.md), so a name
// costs nothing and a mistake is a compile error.
typedef enum {
	PRIM_NONE = 0,

	// -- arithmetic. Receiver and argument may each be a SmallInteger or a
	// Float; anything else fails, and so does any result needing a heap box.
	PRIM_ADD,
	PRIM_SUBTRACT,
	PRIM_MULTIPLY,
	PRIM_DIVIDE,        // exact division only: 7/2 fails, and Fraction is the
	                    // fallback's business, not this file's
	PRIM_FLOOR_DIVIDE,  // //   SmallInteger only
	PRIM_FLOOR_MODULO,  // \\   SmallInteger only

	// -- comparison. Same operand rule; answers true or false.
	PRIM_LESS,
	PRIM_GREATER,
	PRIM_LESS_EQUAL,
	PRIM_GREATER_EQUAL,
	PRIM_NUMERIC_EQUAL,
	PRIM_NUMERIC_NOT_EQUAL,

	// -- bit operations, SmallInteger only
	PRIM_BIT_AND,
	PRIM_BIT_OR,
	PRIM_BIT_XOR,
	PRIM_BIT_SHIFT,     // positive shifts left, negative shifts right

	// -- identity and reflection. These never fail.
	PRIM_IDENTICAL,     // ==
	PRIM_NOT_IDENTICAL, // ~~
	PRIM_CLASS,
	PRIM_IDENTITY_HASH,

	// -- allocation, and the only primitives here that touch the heap. They
	// anchor the calling frame; see the note at the top of this file.
	PRIM_BASIC_NEW,       // an instance with no indexed part
	PRIM_BASIC_NEW_SIZED, // basicNew: n

	// -- indexed access, one per storage format rather than one polymorphic
	// primitive, because the ANSWER's type differs: a String's element is a
	// Character and a ByteArray's is a SmallInteger, and the two share a format.
	// The class that installs the primitive is what decides, which is where the
	// decision belongs.
	PRIM_BASIC_SIZE,
	PRIM_ARRAY_AT,
	PRIM_ARRAY_AT_PUT,
	PRIM_STRING_AT,
	PRIM_STRING_AT_PUT,
	PRIM_BYTES_AT,
	PRIM_BYTES_AT_PUT,

	PRIM_COUNT
} PrimitiveNumber;

// The implementation of a primitive, or NULL for PRIM_NONE. Out-of-range is an
// assertion, not a NULL: a method naming a primitive that does not exist is a
// bug in whoever built the method, and it must not turn into a send that
// quietly does nothing.
PrimitiveFunction primitiveFunctionAt(PrimitiveNumber number);
const char *primitiveName(PrimitiveNumber number);

#endif
