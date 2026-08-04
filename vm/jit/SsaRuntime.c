// The runtime entry points only the SSA BACKEND calls.
//
// Separate from jit/Jit.c on purpose. Gate levels 3, 8 and 9 hand-link Jit.c
// with half a dozen files and no tier 2 at all, so anything tier 2 needs and
// tier 1 does not belongs somewhere those levels do not link. The same seam
// jit/Tier2DryRun.c uses, for the same reason.
//
// WHAT THESE ARE. Boxing and unboxing, as CALLS. Tier 1 never needs them: every
// value it touches is tagged, because a template compiler that put a raw double
// in a frame slot would break the uniform frame map its whole correctness
// argument rests on. Tier 2 keeps raw values, so it needs the two conversions,
// and they are conversions the IR already names (IR_BOX_F and friends).
//
// WHY CALLS AND NOT INLINE SEQUENCES, stated as a first cut rather than as the
// design. A SmallFloat64 is a rotated exponent window with a heap box for
// everything outside it (ADR 0005), so the inline form is a compare, a rotate,
// a branch and an allocation path. Getting it wrong is a wrong answer on
// subnormals, infinities and NaN, which is exactly where a test is least likely
// to look, and the allocation path needs a frame anchor emitted inline. The
// call is correct today and measurably slower; the inline form is a later
// change that comes with a number.
//
// ALL FOUR HAVE THE RUNTIME HELPER SHAPE (jit/Jit.h): a pointer, the address of
// a frame slot, and a packed integer whose high half says WHICH slot that was.
// The slot index is what turns the address back into a frame pointer, which is
// how a collection under the allocating one finds the compiled frames beneath.

#include "jit/Jit.h"
#include "jit/SsaRuntime.h"
#include "core/Assert.h"
#include "core/Handle.h"
#include "core/Object.h"
#include "core/Thread.h"
#include "memory/Heap.h"
#include <string.h>

#define PACKED_SLOT(packed) ((uint16_t) (((packed) >> 32) & 0xFFFF))


// The slot holds the RAW BITS of a double. Answers it tagged.
//
// The slot is described as SLOT_F64 in the frame map at this call, which is
// requirement R1 of ADR 0003 doing its job: without the distinction the
// collector would read this double as a pointer, and a double whose bit pattern
// happens to end in 01 satisfies every plausibility test there is.
Value jitBoxFloat(void *unused, Value *slot, uint64_t packed)
{
	(void) unused;
	double value;
	memcpy(&value, slot, sizeof(value));
	if (smallFloatFits(value)) {
		return tagFloat(value);
	}
	// The boxed half, which allocates and therefore anchors. Before the
	// bootstrap has BoxedFloat64 there is nowhere to put it, and failing loudly
	// beats answering something plausible: that window is the built-in kernel,
	// which has no float literals, so reaching here at all is a bug.
	ASSERT(Handles.BoxedFloat64.raw != NULL);
	CompiledFrameGuard guard;
	compiledFrameEnter(&guard, slot, PACKED_SLOT(packed),
		__builtin_return_address(0));
	Object *box = newObject(&Handles.BoxedFloat64, 0);
	((RawFloat *) box->raw)->value = value;
	Value answer = objectTagged(box);
	compiledFrameLeave(&guard);
	return answer;
}


// The slot holds a TAGGED value. Answers the raw bits of the double in it.
//
// It ASSERTS rather than failing, and the reason is where it can be reached
// from: an IR_UNBOX_F is only ever emitted where the optimizer PROVED the value
// is a float, so a non-float arriving here means the proof was wrong. Answering
// zero would turn a broken proof into a plausible number.
Value jitUnboxFloat(void *unused, Value *slot, uint64_t packed)
{
	(void) unused;
	(void) packed;
	Value tagged = *slot;
	double value;
	if (valueTypeOf(tagged, VALUE_FLOAT)) {
		value = floatValueOf(tagged);
	} else {
		ASSERT(valueTypeOf(tagged, VALUE_POINTER));
		RawObject *object = asObject(tagged);
		ASSERT(Handles.BoxedFloat64.raw != NULL
			&& rawObjectClassIndex(object) == classIndexOf(&Handles.BoxedFloat64));
		value = ((RawFloat *) object)->value;
	}
	Value bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}


// The slot holds a RAW 64-bit integer. Answers it tagged.
//
// A SmallInteger payload is 62 signed bits, so this can overflow, and overflow
// is a HARD FAILURE here rather than a promotion to LargeInteger. Promoting
// would be the wrong place to do it: the general case is a send, which the
// method's own Smalltalk already carries, and an unbox that quietly widened
// would hide the fact that the optimizer produced a raw integer it could not
// prove would fit.
Value jitBoxInteger(void *unused, Value *slot, uint64_t packed)
{
	(void) unused;
	(void) packed;
	intptr_t value = (intptr_t) *slot;
	ASSERT(value >= -((intptr_t) 1 << 61) && value <= (((intptr_t) 1 << 61) - 1));
	return tagInt(value);
}


// The slot holds a TAGGED SmallInteger. Answers its raw value.
Value jitUnboxInteger(void *unused, Value *slot, uint64_t packed)
{
	(void) unused;
	(void) packed;
	ASSERT(valueTypeOf(*slot, VALUE_INT));
	return (Value) (intptr_t) asCInt(*slot);
}
