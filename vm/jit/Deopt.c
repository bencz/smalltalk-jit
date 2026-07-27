#include "jit/Deopt.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "core/Handle.h"
#include "memory/Heap.h"
#include <stdlib.h>


Value deoptMaterialize(uint32_t classIndex, _Bool flat, Value *fields,
	uint16_t fieldCount)
{
	HandleScope scope;
	openHandleScope(&scope);

	Class *class = scopeHandle(classTableAt(&CurrentThread.heap->classes, classIndex));
	Object *object = newObject(class, flat ? fieldCount : 0);

	// Fields are written AFTER the allocation and read from the caller's array
	// each time, because the allocation may have collected and moved anything
	// the array names. Writing them through the barrier matters too: a
	// materialized object can be born old when the nursery is full, and an
	// old-to-young edge the barrier never saw is invisible to the next
	// collection.
	for (uint16_t i = 0; i < fieldCount; i++) {
		Value field = fields[i];
		Value *slot = &((Value *) object->raw->body)[i];
		if (valueTypeOf(field, VALUE_POINTER)) {
			rawObjectStorePtr(object->raw, slot, asObject(field));
		} else {
			*slot = field;
		}
	}

	Value result = tagPtr(object->raw);
	closeHandleScope(&scope, NULL);
	return result;
}


_Bool deoptStateIsWellFormed(const DeoptState *state)
{
	if (state == NULL || state->frameCount == 0) {
		return 0;
	}
	for (uint16_t f = 0; f < state->frameCount; f++) {
		const DeoptFrame *frame = &state->frames[f];
		if (frame->unit == NULL) {
			return 0;
		}
		// Exactly one innermost frame, and it must be the LAST: the ordering is
		// outermost first, and the innermost is the one that re-executes.
		_Bool shouldBeInnermost = (f == state->frameCount - 1);
		if (frame->innermost != shouldBeInnermost) {
			return 0;
		}
		// An outer frame must say where the in-flight call's result goes. A
		// frame that resumes after a send without a destination would discard
		// the result of the call it is resuming from.
		if (!frame->innermost && frame->destRegister == BYTECODE_NO_TARGET) {
			return 0;
		}
		for (uint16_t s = 0; s < frame->slotCount; s++) {
			if (frame->slotValue[s] == NULL) {
				return 0;
			}
		}
	}
	return 1;
}


_Bool deoptStressEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_DEOPT_STRESS") != NULL;
	}
	return enabled;
}
