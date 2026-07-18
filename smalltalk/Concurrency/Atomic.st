"A generic atomic cell: one slot mutated with lock-free machine atomics
 (load / store / compare-and-set / exchange) instead of the global sync monitor.

 It holds ANY value. Reads use an acquire load, writes a release store, and the
 read-modify-write operations (getAndSet:, compareAndSet:with:) a full acquire+
 release barrier, so a value published by set:/compareAndSet: is fully visible to a
 later get on another worker. Correct on x86-64 (TSO) and on weakly-ordered POWER
 alike (the ordering is carried by the primitives, not by the memory model).

 Storing a heap object runs the generational write barrier, so a cell may safely
 hold references to young objects across a GC. The collector is stop-the-world at
 safepoints, so an in-flight atomic never races a moving collector.

 This is a runtime building block, not an actor detail: use it for id counters,
 flags, sequence locks, and lock-free data structures (a Treiber stack is just a
 cell of nodes updated with compareAndSet:with:). AtomicInteger adds fetch-add for
 the common counter case."
Atomic := Object [

	| value |


	class new [
		^super new initAtomic
	]


	class on: aValue [
		^super new setValue: aValue
	]


	initAtomic [
		"nil is a permanent (old-space) object, so publishing it never needs a barrier."
		value := nil.
		^self
	]


	setValue: aValue [
		self set: aValue.
		^self
	]


	"acquire load"
	get [
		<primitive: AtomicLoadPrimitive>
		^value
	]


	"release store (+ write barrier when aValue is a heap object)"
	set: aValue [
		<primitive: AtomicStorePrimitive>
		value := aValue.
		^self
	]


	"If the cell holds expected, replace it with newValue and answer true; else answer
	 false. Comparison is by identity of the stored word (identity for objects, value
	 for immediates)."
	compareAndSet: expected with: newValue [
		<primitive: AtomicCompareAndSetPrimitive>
		value == expected ifTrue: [ value := newValue. ^true ].
		^false
	]


	"Store aValue and answer the previous value."
	getAndSet: aValue [
		<primitive: AtomicGetAndSetPrimitive>
		| old |
		old := value.
		value := aValue.
		^old
	]


	value [ ^self get ]

	value: aValue [ ^self set: aValue ]


	printOn: aStream [
		aStream nextPutAll: 'Atomic('.
		self get printOn: aStream.
		aStream nextPutAll: ')'.
	]
]
