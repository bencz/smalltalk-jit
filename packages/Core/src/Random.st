"A Park-Miller / Lehmer MINSTD pseudo-random generator:

     state := state * 48271 \\ 2147483647

 The whole hot path (nextValue and everything built on it) stays inside a
 62-bit SmallInteger: 2147483646 * 48271 is about 1.0e14, far below the
 tagged-integer ceiling, so no LargeInteger is ever created. The seed lives in
 1..2147483646 (state 0 is the Lehmer fixed point and is forbidden).

 `Random new` seeds from OS entropy (8 bytes via RandomBytesPrimitive, folded
 modulo the generator modulus so it never overflows into a LargeInteger),
 falling back to the current microsecond clock if the primitive is unavailable.
 `Random seed:` gives a deterministic generator for tests. `Random default` is
 a lazily created shared instance used by the collection shuffle/atRandom
 helpers."

Random := Object [

	| seed |


	"instance creation"

	class new [
		^self basicNew seedFromEntropy
	]


	class seed: anInteger [
		^self basicNew setSeed: anInteger
	]


	class default [
		RandomDefault isNil ifTrue: [RandomDefault := self new].
		^RandomDefault
	]


	"initialization"

	seedFromEntropy [
		| bytes value ok |

		bytes := ByteArray new: 8.
		ok := self primitiveRandomBytes: bytes.
		ok isNil
			ifTrue: [^self setSeed: DateTime currentMicroTime]
			ifFalse: [
				value := 0.
				1 to: 8 do: [ :i | value := (value * 256 + (bytes at: i)) \\ 2147483647].
				^self setSeed: value]
	]


	primitiveRandomBytes: aByteArray [
		<primitive: RandomBytesPrimitive>
		^nil
	]


	setSeed: anInteger [
		"Normalise any integer into the valid state range 1..2147483646."
		| s |

		s := anInteger.
		s < 0 ifTrue: [s := s negated].
		s := s \\ 2147483647.
		s = 0 ifTrue: [s := 1].
		seed := s.
		^self
	]


	"generating"

	nextValue [
		"Advance the state and answer it (an integer in 1..2147483646)."
		seed := seed * 48271 \\ 2147483647.
		^seed
	]


	next [
		"A Float uniformly distributed in [0, 1)."
		^(self nextValue - 1) asFloat / 2147483647
	]


	nextInteger: n [
		"An integer uniformly in 1..n."
		n < 1 ifTrue: [^self error: 'nextInteger: requires a positive argument'].
		^(self nextValue \\ n) + 1
	]


	between: lowerInteger and: upperInteger [
		"An integer uniformly in lowerInteger..upperInteger, inclusive."
		^lowerInteger + (self nextInteger: upperInteger - lowerInteger + 1) - 1
	]


	nextBoolean [
		^(self nextValue bitAnd: 1) = 0
	]

]
