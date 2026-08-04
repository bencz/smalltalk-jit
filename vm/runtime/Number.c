#include "runtime/Number.h"
#include "core/Thread.h"
#include "memory/Heap.h"


Float *newFloat(double value)
{
	Float *boxed = newObject(&Handles.BoxedFloat64, 0);
	// FORMAT_NO_POINTERS: the body is one raw word and the collector never looks
	// at it, which is what lets a mantissa that happens to look like a tagged
	// pointer sit here safely.
	boxed->raw->value = value;
	return boxed;
}


Value floatValue(double value)
{
	return smallFloatFits(value) ? tagFloat(value) : objectTagged(newFloat(value));
}
