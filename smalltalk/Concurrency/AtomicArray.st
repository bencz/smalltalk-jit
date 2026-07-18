"A fixed-size array of atomic slots: per-index lock-free load / store / CAS /
 exchange / fetch-add, the indexed sibling of Atomic. Wraps a plain Array (so no new
 indexed object format is needed); each `at: i ...` operates atomically on element i
 with the same memory ordering and write barrier as Atomic (acquire load, release
 store, acq_rel RMW), correct on x86-TSO and POWER.

 The building block for lock-free structures with per-slot contention: ring buffers,
 work-stealing deques, striped counters, sharded flags. Indices are 1-based; slots
 start as nil (integer slots should be initialized with at:put: before getAndAdd:)."
AtomicArray := Object [

	| slots |


	class new: anInteger [
		^super new initSlots: anInteger
	]


	initSlots: anInteger [
		slots := Array new: anInteger.
		^self
	]


	size [ ^slots size ]


	"acquire load of element i"
	at: i [
		<primitive: AtomicArrayAtPrimitive>
		^self indexError: i
	]


	"release store into element i (+ write barrier on a pointer publish)"
	at: i put: aValue [
		<primitive: AtomicArrayAtPutPrimitive>
		^self indexError: i
	]


	"if element i holds expected, replace with newValue and answer true; else false"
	at: i compareAndSet: expected with: newValue [
		<primitive: AtomicArrayCompareAndSetPrimitive>
		^self indexError: i
	]


	"store aValue into element i, answer its previous value"
	at: i getAndSet: aValue [
		<primitive: AtomicArrayGetAndSetPrimitive>
		^self indexError: i
	]


	"atomically add anInteger to element i (a SmallInteger slot), answer the old value"
	at: i getAndAdd: anInteger [
		<primitive: AtomicArrayGetAndAddPrimitive>
		(anInteger isKindOf: SmallInteger)
			ifFalse: [ ^self error: 'AtomicArray getAndAdd: needs a SmallInteger' ].
		^self indexError: i
	]


	incrementAndGetAt: i [ ^(self at: i getAndAdd: 1) + 1 ]

	getAndIncrementAt: i [ ^self at: i getAndAdd: 1 ]


	indexError: i [
		^self error: 'AtomicArray index out of bounds: ', i printString
	]
]
