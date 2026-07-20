"An Atomic specialized for SmallInteger counters: adds a lock-free fetch-add on top
 of the generic load / store / compare-and-set it inherits.

 The slot always holds a tagged SmallInteger, so getAndAdd: adds the tagged delta
 word to the slot in one atomic step (the tag bits are preserved). This is the
 monitor-free replacement for a `counter := counter + 1` that races across workers.
 Range is 62-bit (a raw atomic add does not promote to LargeInteger on overflow),
 which is unbounded for any practical id/counter use."
AtomicInteger := Atomic [

	class new [
		^super new setValue: 0
	]


	class on: anInteger [
		^super new setValue: anInteger
	]


	"Atomically add anInteger and answer the PREVIOUS value."
	getAndAdd: anInteger [
		<primitive: AtomicGetAndAddPrimitive>
		^self error: 'AtomicInteger getAndAdd: needs a SmallInteger'
	]


	"Atomically add anInteger and answer the NEW value."
	addAndGet: anInteger [
		^(self getAndAdd: anInteger) + anInteger
	]


	getAndIncrement [ ^self getAndAdd: 1 ]

	incrementAndGet [ ^self addAndGet: 1 ]

	getAndDecrement [ ^self getAndAdd: -1 ]

	decrementAndGet [ ^self addAndGet: -1 ]
]
