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


// Log an old-to-young edge WITHOUT performing the store.
//
// The barrier in core/Thread.h remembers and stores together, which is right
// everywhere a store is a plain assignment. Here it is not: the store IS the
// atomic instruction, and it cannot also be an assignment without stopping being
// atomic. So the two halves are separated and the remembering happens FIRST.
//
// FIRST, and conservatively, on purpose. Remembering an edge that the compare
// then declines to install costs one entry that the next collection drops again
// (scavengeRememberedSet re-adds only what still points young). Remembering
// afterwards would leave a window in which the edge exists and no log names it,
// and a peer collecting inside that window reclaims a live object -- the failure
// this barrier exists to prevent, reintroduced by the ordering.
static void atomicRemember(RawObject *object, Value value)
{
	if (valueTypeOf(value, VALUE_POINTER) && isOldObject(object)
			&& isNewObject(asObject(value))
			&& !rawObjectHasGcBit(object, GC_REMEMBERED)) {
		rememberedSetAdd(&CurrentThread.rememberedSet, object);
	}
}


// Store through the generational barrier when the value is a heap pointer, and
// plainly when it is an immediate. One place, because every store below needs
// exactly this and getting it wrong is a young object an old cell holds that
// the collector never hears about.
//
// The store itself is a RELEASE store rather than a fence and an assignment: on
// this side the two are equivalent, and writing it as one operation is what keeps
// it obviously indivisible next to the compare-and-set below.
static void atomicStore(RawObject *object, Value *slot, Value value)
{
	atomicRemember(object, value);
	__atomic_store_n(slot, value, __ATOMIC_RELEASE);
}


// Install `desired` only if the slot still holds `expected`, as ONE indivisible
// operation. Answers whether it did.
//
// A REAL COMPARE-AND-EXCHANGE, which is the whole point of the facility and which
// this file did not have: it compared, and then stored, as two separate accesses
// with nothing joining them. Two threads that both saw `expected` therefore both
// stored, and the second silently overwrote the first.
//
// A fence does not fix that and there was one here. A fence orders the accesses
// one thread makes; it does not stop another thread from getting between them. The
// symptom is not a crash: a Treiber stack built on this loses and duplicates
// nodes, so the count and the sum come out wrong. Measured, by the multi-worker
// Atomics stress -- which is also the only thing in the suite that could see it,
// since a single thread never has a competitor to lose to.
static _Bool atomicCompareAndSet(RawObject *object, Value *slot, Value expected,
	Value desired)
{
	atomicRemember(object, desired);
	Value witness = expected;
	return __atomic_compare_exchange_n(slot, &witness, desired, 0,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}


// Replace the slot and answer what it held, indivisibly. Same defect as above,
// same reason it matters: a read followed by a store lets a value written between
// the two vanish without ever being answered to anybody.
static Value atomicExchange(RawObject *object, Value *slot, Value value)
{
	atomicRemember(object, value);
	return __atomic_exchange_n(slot, value, __ATOMIC_ACQ_REL);
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
// A CAS LOOP and not __atomic_fetch_add, because the range check has to be part
// of the operation. A tagged SmallInteger is a shifted payload, so adding the two
// tagged words would give the right answer for every pair that fits and a wrapped
// one for every pair that does not -- and "wrapped silently at 2^61" is the exact
// wrong answer the check below exists to refuse. The check has to be re-evaluated
// against the value the exchange actually observed, which is what a loop does and
// what a single fetch-add cannot.
//
// It was a plain read-modify-write with a fence in the middle. That is not atomic
// at all: two threads both read the same `before`, both compute the same sum, and
// one increment is simply gone. Measured on the contended AtomicArray slot.
static _Bool atomicAdd(RawObject *object, Value *slot, Value delta, Value *previous)
{
	if (!valueTypeOf(delta, VALUE_INT)) {
		return 0;
	}
	(void) object; // the result is an immediate, so no edge to remember
	intptr_t step = asCInt(delta);
	Value before = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
	for (;;) {
		if (!valueTypeOf(before, VALUE_INT)) {
			return 0;
		}
		intptr_t sum = (intptr_t) ((uintptr_t) asCInt(before) + (uintptr_t) step);
		if (!smallIntFits(sum)) {
			return 0;
		}
		// On failure the witness is updated with what the slot really held, so the
		// next attempt re-checks the range against that rather than against a value
		// no longer there.
		if (__atomic_compare_exchange_n(slot, &before, tagInt(sum), 0,
				__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*previous = before;
			return 1;
		}
	}
}


// Atomic>>get
Value primAtomicLoad(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *object;
	if (argc != 0 || !atomicCell(primitiveReceiver(args), &slot, &object)) {
		return PRIMITIVE_FAILED;
	}
	// An ACQUIRE LOAD rather than a plain read followed by a fence. The fence
	// gives the ordering, but the read it is meant to order is an ordinary C load
	// that the compiler is free to move, split or repeat; spelling the whole thing
	// as one atomic operation is what makes the guarantee the facility promises.
	return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
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
	return booleanResult(atomicCompareAndSet(object, slot,
		primitiveArgument(args, 0), primitiveArgument(args, 1)));
}


// Atomic>>getAndSet: aValue
Value primAtomicGetAndSet(Value *args, uint64_t argc)
{
	Value *slot;
	RawObject *object;
	if (argc != 1 || !atomicCell(primitiveReceiver(args), &slot, &object)) {
		return PRIMITIVE_FAILED;
	}
	return atomicExchange(object, slot, primitiveArgument(args, 0));
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
	// An ACQUIRE LOAD rather than a plain read followed by a fence. The fence
	// gives the ordering, but the read it is meant to order is an ordinary C load
	// that the compiler is free to move, split or repeat; spelling the whole thing
	// as one atomic operation is what makes the guarantee the facility promises.
	return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
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
	return booleanResult(atomicCompareAndSet(array, slot,
		primitiveArgument(args, 1), primitiveArgument(args, 2)));
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
	return atomicExchange(array, slot, primitiveArgument(args, 1));
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
