SmallInteger := Integer [

	"--- range ---"

	class maxVal [
		"Largest value that stays immediate. A Value carries a 2-bit tag, so the
		 payload is a SIGNED 62-bit field: [-2^61, 2^61-1]. Anything past this
		 promotes to LargePositiveInteger, and LargeInteger>>normalize demotes at
		 exactly this boundary. WARNING: the range is asymmetric, so
		 `SmallInteger maxVal negated` is NOT minVal."
		^2305843009213693951
	]


	class minVal [
		"Computed, not written as a literal: -2^61 is exactly one past the
		 parser's SYMMETRIC demotion boundary (|v| < 2^61), so as a literal it
		 would be built as a LargeNegativeInteger, and this file is loaded before
		 LargeNegativeInteger.st even exists. maxVal negated is comfortably
		 immediate and the subtraction lands exactly on the payload minimum."
		^self maxVal negated - 1
	]


	"comparing"

	hash [
		^self
	]


	identityHash [
		^self
	]


	= aNumber [
		(aNumber isKindOf: SmallInteger) ifTrue: [^self == aNumber].
		"An Integer argument is handled HERE, never through retry:coercing:.
		 LargeInteger>>coerce: answers its argument unchanged (there is nothing
		 to widen, the kernels take a SmallInteger directly), so a pair that
		 reached retry: would be re-dispatched identically and recur forever."
		(aNumber isKindOf: Integer) ifTrue: [^(self largeCompare: aNumber) = 0].
		(aNumber isKindOf: Number) ifTrue: [^self retryRelationalOp: #= coercing: aNumber].
		^false
	]


	< aNumber [
		 <primitive: IntLessThanPrimitive>
		"See = below for why an Integer argument must not reach retry:coercing:.
		 Magnitude derives >, <= and >= from this one, so fixing < and = covers
		 the whole relational protocol."
		(aNumber isKindOf: Integer) ifTrue: [^(self largeCompare: aNumber) < 0].
		^self retryRelationalOp: #< coercing: aNumber
	]


	"arithmetic"

	+ aNumber [
		<primitive: IntAddPrimitive>
		(aNumber isKindOf: Integer) ifTrue: [^self largeOp: 1 with: aNumber].
		^self retry: #+ coercing: aNumber
	]


	- aNumber [
		<primitive: IntSubPrimitive>
		(aNumber isKindOf: Integer) ifTrue: [^self largeOp: 2 with: aNumber].
		^self retry: #- coercing: aNumber
	]


	* aNumber [
		<primitive: IntMulPrimitive>
		(aNumber isKindOf: Integer) ifTrue: [^self largeOp: 3 with: aNumber].
		^self retry: #* coercing: aNumber
	]


	/ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^ZeroDivide new signal].
			^Fraction numerator: self denominator: aNumber].
		^self retry: #/ coercing: aNumber
	]


	\\ aNumber [
		<primitive: IntModPrimitive>
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^ZeroDivide new signal].
			^self largeOp: 7 with: aNumber].
		^self retry: #\\ coercing: aNumber
	]


	quo: aNumber [
		<primitive: IntQuoPrimitive>
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^ZeroDivide new signal].
			^self largeOp: 4 with: aNumber].
		^self retry: #quo: coercing: aNumber
	]


	rem: aNumber [
		<primitive: IntRemPrimitive>
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^ZeroDivide new signal].
			^self largeOp: 5 with: aNumber].
		^self retry: #rem: coercing: aNumber
	]


	negated [
		<primitive: IntNegPrimitive>
		^self primitiveFailedFor: self.
	]


	"bit manipulation"

	bitAnd: anInteger [
		<primitive: IntAndPrimitive>
		^self primitiveFailedFor: anInteger.
	]


	bitOr: anInteger [
		<primitive: IntOrPrimitive>
		^self primitiveFailedFor: anInteger.
	]


	bitXor: anInteger [
		<primitive: IntXorPrimitive>
		^self primitiveFailedFor: anInteger.
	]


	bitShift: anInteger [
		"The primitive handles every right shift (the count is clamped, and an
		 arithmetic shift saturates to the exact answer) and every left shift
		 that still fits 62 bits. It fails only on overflow, where the answer is
		 a LargeInteger, so PROMOTE instead of raising: `1 bitShift: 100` used to
		 answer 2^36 because x86 masks the shift count to 6 bits."
		<primitive: IntShiftPrimitive>
		(anInteger isKindOf: Integer) ifFalse: [^self primitiveFailedFor: anInteger].
		^self largeShift: anInteger
	]


	bitInvert [
		^self bitXor: -1
	]


	highBit [
		| n bit |

		self = 0 ifTrue: [^0].

		bit := 0.
		self < 0
			ifTrue: [
				"Increment the result by one if not a power of two"
				n := self negated.
				(n bitAnd: self) = n ifFalse: [bit := 1]]
			ifFalse: [
				n := self.
				bit := 0].

		[n > 16r1FFFFFFFFFFFFFFF] whileTrue: [
			bit := bit + 62.
			n := n bitShift: -62].

		n > 16rFFFFFFFF ifTrue: [
			bit := bit + 32.
			n := n bitShift: -32].

		n > 16rFFFF ifTrue: [
			bit := bit + 16.
			n := n bitShift: -16].

		n > 16rFF ifTrue: [
			bit := bit + 8.
			n := n bitShift: -8].

		n > 16rF ifTrue: [
			bit := bit + 4.
			n := n bitShift: -4].

		n > 3 ifTrue: [
			bit := bit + 2.
			n := n bitShift: -2].

		n > 1 ifTrue: [
			bit := bit + 1.
			n := n bitShift: -1].

		^n + bit
	]


	"private"

	zero [
		^0
	]


	unity [
		 ^1
	]


	asObject [
		"The IntAsObjectPrimitive is unimplemented and its stub hard-exits the VM, so
		 raise a catchable error instead of reaching it."
		^self shouldNotImplement
	]


	"coercing"

	generality [
		^100
	]


	coerce: aNumber [
		"No number has lower generality, so this is only ever a no-op."
		^aNumber
	]


	asFloat [
		<primitive: IntAsFloatPrimitive>
	]


	asLargeInteger [
		"Answers SELF, deliberately: the value already fits the immediate
		 payload, and every large-integer kernel accepts a SmallInteger operand
		 directly, so widening would allocate an object that normalization would
		 immediately undo. This used to build a base-2^16 limb Array by repeated
		 quo:/rem:, which was the slow half of every overflow.

		 WARNING: because this does NOT raise the receiver's generality, it must
		 never be the thing that makes retry:coercing: converge. Every Integer
		 operation short-circuits an Integer argument before reaching retry:
		 (see + above), which is what keeps that true."
		^self
	]


	truncated [
		^self
	]


	floor [
		^self
	]


	ceiling [
		^self
	]


	rounded [
		^self
	]


	primitiveFailedFor: aNumber [
		(aNumber isKindOf: Number) ifFalse: [Error signal: aNumber asString, ' is not a Number'].
		Error signal: 'Integer overflow'. "TODO: add support for large integer"
	]


	"copying"

	shallowCopy [
		^self
	]


	deepCopy [
		^self
	]


	"testing"

	even [
		^(self bitAnd: 1) = 0
	]


	odd [
		^(self bitAnd: 1) = 1
	]


	"printing"

	printOn: aStream [
		self printOn: aStream base: 10.
	]


	printOn: aStream base: base [
		aStream nextPutAll: (self printStringBase: base).
	]


	printString [
		^self printStringBase: 10
	]


	printStringBase: base [
		| int string i |

		self < 0
			ifTrue: [
				int := self negated.
				i := (int floorLog: base) + 2.
				string := String new: i.
				string at: 1 put: $-]
			ifFalse: [
				int := self.
				i := (int floorLog: base) + 1.
				string := String new: i].

		[string at: i put: (Character digitValue: int \\ base).
		i := i - 1.
		(int := int // base) > 0]
			whileTrue.

		^string
	]

]
