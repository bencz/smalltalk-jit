// Memory ordering.
//
// WHAT IS HERE IS ONE FENCE, and the rest of the Atomic family
// (AtomicLoad/Store/CompareAndSet/GetAndAdd/GetAndSet and the AtomicArray five)
// is still PRIMITIVE_ABSENT. That is not a stopping point chosen at random: a
// fence is meaningful in a single-threaded scheduler and an atomic cell is not
// yet, because there is nothing to be atomic against.
//
// THE FENCE STILL HAS TO EXIST, and the reason is visible rather than
// theoretical. ConcurrentDictionary publishes a copy-on-write snapshot:
//
//     copy := self copyOfSnapshot.
//     copy at: key put: anObject.
//     Atomic releaseFence.
//     snapshot := copy.
//
// With the primitive absent, `releaseFence` raised INSIDE the critical section,
// so `Processor monitorExit` below it never ran, the monitor stayed held, and
// the next write from the same fiber failed as a re-entry. One missing fence
// took out every write path in the class, and the error it produced named the
// monitor rather than the fence.

#include "runtime/primitives/Shared.h"

// ---------------------------------------------------------------------------
// The cells
// ---------------------------------------------------------------------------
//
// `Atomic` is one named slot and `AtomicArray` is one named slot holding a
// plain Array, so neither needs an object format of its own -- which is what
// their .st files say and why the operations below are slot arithmetic rather
// than a new shape.
//
// WHAT "ATOMIC" MEANS ON THIS SCHEDULER, said plainly: fibers are cooperative
// and single-threaded (concurrency/Scheduler.h), so no operation here can be
// interleaved with another and the read-modify-writes are atomic by
// construction. The FENCES are still emitted, for the same reason releaseFence
// is: what they order is a publication, and that requirement does not change
// when workers arrive -- only the cost does.
//
// THE WRITE BARRIER IS NOT FREE, and it is the part that matters today. A cell
// that has been promoted holding a reference to a young object is exactly what
// the remembered set exists for, so every store of a POINTER goes through
// rawObjectStorePtr and every store of an immediate does not.

// The single named slot of an Atomic, or of an AtomicArray.
//
// BY SHAPE, through the class: a named slot lives at body word 0 of a
// FORMAT_POINTERS object, and how many there are is the class's business. A
// receiver of any other shape fails rather than having word 0 read as a Value,
// which for an indexed object would be its element COUNT.
static _Bool atomicCell(Value receiver, Value **slot, RawObject **object)
{
	if (!valueTypeOf(receiver, VALUE_POINTER)) {
		return 0;
	}
	RawObject *raw = asObject(receiver);
	if (rawObjectFormat(raw) != FORMAT_POINTERS) {
		return 0;
	}
	RawClass *class = classOf(receiver);
	if (class == NULL || class->instanceShape.fixedSlots < 1) {
		return 0;
	}
	*slot = (Value *) raw->body;
	*object = raw;
	return 1;
}


// Store through the generational barrier when the value is a heap pointer, and
// plainly when it is an immediate. One place, because every store below needs
// exactly this and getting it wrong is a young object an old cell holds that
// the collector never hears about.
static void atomicStore(RawObject *object, Value *slot, Value value)
{
	__atomic_thread_fence(__ATOMIC_RELEASE);
	if (valueTypeOf(value, VALUE_POINTER)) {
		rawObjectStorePtr(object, slot, asObject(value));
	} else {
		*slot = value;
	}
}


// The element slot of an AtomicArray: its named slot holds a plain Array, and
// `index` is 1-based, as every indexed accessor in this system is.
static _Bool atomicElement(Value receiver, Value indexValue, Value **slot,
	RawObject **array)
{
	Value *cell;
	RawObject *object;
	if (!atomicCell(receiver, &cell, &object) || !valueTypeOf(indexValue, VALUE_INT)) {
		return 0;
	}
	if (!valueTypeOf(*cell, VALUE_POINTER)) {
		return 0;
	}
	RawObject *raw = asObject(*cell);
	if (rawObjectFormat(raw) != FORMAT_INDEXED_POINTERS) {
		return 0;
	}
	intptr_t index = asCInt(indexValue);
	if (index < 1 || (size_t) index > rawObjectElementCount(raw)) {
		return 0; // the kernel's indexError: raises
	}
	*slot = &rawObjectIndexedPointers(raw)[index - 1];
	*array = raw;
	return 1;
}


// Fetch-add over a slot, shared by AtomicInteger and AtomicArray.
//
// BOTH OPERANDS MUST BE SmallIntegers and the SUM MUST FIT one, which is not
// pedantry: this is the counter path, and a counter that silently wrapped into
// a negative at 2^61 would be a wrong answer nobody looks for. Overflow fails
// into the Smalltalk fallback, which says so.
static _Bool atomicAdd(RawObject *object, Value *slot, Value delta, Value *previous)
{
	if (!valueTypeOf(*slot, VALUE_INT) || !valueTypeOf(delta, VALUE_INT)) {
		return 0;
	}
	intptr_t before = asCInt(*slot);
	intptr_t sum = (intptr_t) ((uintptr_t) before + (uintptr_t) asCInt(delta));
	if (!smallIntFits(sum)) {
		return 0;
	}
	*previous = *slot;
	(void) object; // an immediate store needs no barrier
	__atomic_thread_fence(__ATOMIC_ACQ_REL);
	*slot = tagInt(sum);
	return 1;
}


// Atomic>>get
Value primAtomicLoad(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *object;
	if (argc != 0 || !atomicCell(primitiveReceiver(args), &slot, &object)) {
		return PRIMITIVE_FAILED;
	}
	Value value = *slot;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return value;
}


// Atomic>>set: aValue
Value primAtomicStore(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *object;
	if (argc != 1 || !atomicCell(primitiveReceiver(args), &slot, &object)) {
		return PRIMITIVE_FAILED;
	}
	atomicStore(object, slot, primitiveArgument(args, 0));
	return primitiveReceiver(args);
}


// Atomic>>compareAndSet: expected with: newValue
//
// BY IDENTITY OF THE STORED WORD, which the kernel states: identity for objects
// and value for immediates, both of which are one compare of the tagged word.
Value primAtomicCompareAndSet(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *object;
	if (argc != 2 || !atomicCell(primitiveReceiver(args), &slot, &object)) {
		return PRIMITIVE_FAILED;
	}
	if (*slot != primitiveArgument(args, 0)) {
		return booleanResult(0);
	}
	atomicStore(object, slot, primitiveArgument(args, 1));
	return booleanResult(1);
}


// Atomic>>getAndSet: aValue
Value primAtomicGetAndSet(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *object;
	if (argc != 1 || !atomicCell(primitiveReceiver(args), &slot, &object)) {
		return PRIMITIVE_FAILED;
	}
	Value previous = *slot;
	atomicStore(object, slot, primitiveArgument(args, 0));
	return previous;
}


// AtomicInteger>>getAndAdd: anInteger
Value primAtomicGetAndAdd(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *object;
	if (argc != 1 || !atomicCell(primitiveReceiver(args), &slot, &object)) {
		return PRIMITIVE_FAILED;
	}
	Value previous;
	if (!atomicAdd(object, slot, primitiveArgument(args, 0), &previous)) {
		return PRIMITIVE_FAILED;
	}
	return previous;
}


// AtomicArray>>at: i
Value primAtomicArrayAt(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *array;
	if (argc != 1
			|| !atomicElement(primitiveReceiver(args), primitiveArgument(args, 0),
				&slot, &array)) {
		return PRIMITIVE_FAILED;
	}
	Value value = *slot;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return value;
}


// AtomicArray>>at: i put: aValue
Value primAtomicArrayAtPut(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *array;
	if (argc != 2
			|| !atomicElement(primitiveReceiver(args), primitiveArgument(args, 0),
				&slot, &array)) {
		return PRIMITIVE_FAILED;
	}
	atomicStore(array, slot, primitiveArgument(args, 1));
	return primitiveArgument(args, 1);
}


// AtomicArray>>at: i compareAndSet: expected with: newValue
Value primAtomicArrayCompareAndSet(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *array;
	if (argc != 3
			|| !atomicElement(primitiveReceiver(args), primitiveArgument(args, 0),
				&slot, &array)) {
		return PRIMITIVE_FAILED;
	}
	if (*slot != primitiveArgument(args, 1)) {
		return booleanResult(0);
	}
	atomicStore(array, slot, primitiveArgument(args, 2));
	return booleanResult(1);
}


// AtomicArray>>at: i getAndSet: aValue
Value primAtomicArrayGetAndSet(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *array;
	if (argc != 2
			|| !atomicElement(primitiveReceiver(args), primitiveArgument(args, 0),
				&slot, &array)) {
		return PRIMITIVE_FAILED;
	}
	Value previous = *slot;
	atomicStore(array, slot, primitiveArgument(args, 1));
	return previous;
}


// AtomicArray>>at: i getAndAdd: anInteger
Value primAtomicArrayGetAndAdd(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *array;
	if (argc != 2
			|| !atomicElement(primitiveReceiver(args), primitiveArgument(args, 0),
				&slot, &array)) {
		return PRIMITIVE_FAILED;
	}
	Value previous;
	if (!atomicAdd(array, slot, primitiveArgument(args, 1), &previous)) {
		return PRIMITIVE_FAILED;
	}
	return previous;
}


// Atomic class>>releaseFence
//
// A COMPILER AND CPU BARRIER, and on this scheduler the first is the part that
// does the work: fibers are cooperative and single-threaded (concurrency/
// Scheduler.h), so no other mutator can observe the store it orders. It is
// still emitted rather than made a no-op, because what it orders is a
// PUBLICATION -- the fields of a freshly built object against the pointer that
// makes it visible -- and that ordering is the same requirement the day workers
// arrive. A no-op today would be a no-op nobody remembered to fill in then.
//
// Nothing here allocates or switches, so there is no frame to anchor.
Value primMemoryReleaseFence(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);
	return primitiveReceiver(args);
}
