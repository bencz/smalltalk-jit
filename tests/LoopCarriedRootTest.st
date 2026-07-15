"Regression test: GC stackmap coverage for LOOP-CARRIED frame slots.

 The linear-scan liveness ranges are textual and control-flow-blind: a method
 temp whose last textual use sits at the TOP of a loop body (e.g. a Boolean
 flag tested by an inlined ifTrue:) used to be omitted from the GC stackmap of
 every allocating send later in the body. A scavenge at such a send left the
 temp's frame slot pointing into the abandoned semispace; the next iteration
 read a dangling object — an inlined boolean test then dispatched
 #mustBeBoolean on it and corrupted lookup state (Dictionary.c:142 aborts).
 Fixed by Variable.gcEnd: extendLoopVarRanges (RegisterAllocator.c) widens the
 stackmap bound of every range overlapping a backward jump to the back-edge.

 This must run in real METHODS: top-level block temps become heap context vars,
 which the GC already scanned precisely.

 Pre-fix this file aborts ~100%; post-fix it completes with exact results."

LoopCarried := Object [
	"the distilled shape: flag's last textual use precedes the allocating send"
	class booleanFlag [
		| flag count junk |
		flag := true.
		count := 0.
		1 to: 2000000 do: [:i |
			flag ifTrue: [ count := count + 1 ] ifFalse: [ count := count + 2 ].
			junk := Array new: 64.
			junk at: 1 put: i ].
		^count
	]

	"same hole with a young ARRAY read early in the body (names in the original
	 reflective JSON encoder), then heavy allocation before the back-edge"
	class earlyArrayRead [
		| names sum s |
		names := Array with: 'alpha' with: 'beta' with: 'gamma'.
		sum := 0.
		1 to: 300000 do: [:i |
			sum := sum + (names at: (i \\ 3) + 1) size.
			s := CollectionStream with: (String new: 32).
			s nextPutAll: 'xxxxxxxxxxxxxxxx'.
			s contents ].
		^sum
	]
]

[
	Assert true: LoopCarried booleanFlag = 2000000.
	Assert true: LoopCarried earlyArrayRead = 1400000.
	Transcript nextPutAll: 'LoopCarriedRootTest: OK'; lf.
]
