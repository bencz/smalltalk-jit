SmallInteger := Integer [

	"comparing"

	hash [
		^self
	]


	identityHash [
		^self
	]


	= aNumber [
		(aNumber isKindOf: SmallInteger) ifTrue: [^self == aNumber].
		(aNumber isKindOf: Number) ifTrue: [^self retryRelationalOp: #= coercing: aNumber].
		^false
	]


	< aNumber [
		 <primitive: IntLessThanPrimitive>
		^self retryRelationalOp: #< coercing: aNumber
	]


	"arithmetic"

	+ aNumber [
		<primitive: IntAddPrimitive>
		(aNumber isKindOf: SmallInteger) ifTrue: [^self asLargeInteger + aNumber asLargeInteger].
		^self retry: #+ coercing: aNumber
	]


	- aNumber [
		<primitive: IntSubPrimitive>
		(aNumber isKindOf: SmallInteger) ifTrue: [^self asLargeInteger - aNumber asLargeInteger].
		^self retry: #- coercing: aNumber
	]


	* aNumber [
		<primitive: IntMulPrimitive>
		(aNumber isKindOf: SmallInteger) ifTrue: [^self asLargeInteger * aNumber asLargeInteger].
		^self retry: #* coercing: aNumber
	]


	/ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^self error: 'division by zero'].
			^Fraction numerator: self denominator: aNumber].
		^self retry: #/ coercing: aNumber
	]


	\\ aNumber [
		<primitive: IntModPrimitive>
		(aNumber isKindOf: SmallInteger) ifTrue: [
			aNumber = 0 ifTrue: [^self error: 'division by zero'].
			^self asLargeInteger \\ aNumber asLargeInteger].
		^self retry: #\\ coercing: aNumber
	]


	quo: aNumber [
		<primitive: IntQuoPrimitive>
		(aNumber isKindOf: SmallInteger) ifTrue: [
			aNumber = 0 ifTrue: [^self error: 'division by zero'].
			^self asLargeInteger quo: aNumber asLargeInteger].
		^self retry: #quo: coercing: aNumber
	]


	rem: aNumber [
		<primitive: IntRemPrimitive>
		(aNumber isKindOf: SmallInteger) ifTrue: [
			aNumber = 0 ifTrue: [^self error: 'division by zero'].
			^self asLargeInteger rem: aNumber asLargeInteger].
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
		<primitive: IntShiftPrimitive>
		^self primitiveFailedFor: anInteger.
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
		"Answer the LargeInteger with the same value. Uses truncating quo:/rem:
		so it is safe for the most-negative SmallInteger too."
		| count v limbs |

		self = 0 ifTrue: [^LargeInteger digits: (Array with: 0) negative: false].

		count := 0.
		v := self.
		[v = 0] whileFalse: [count := count + 1. v := v quo: 65536].

		limbs := Array new: count.
		v := self.
		1 to: count do: [ :i |
			limbs at: i put: (v rem: 65536) abs.
			v := v quo: 65536].
		^LargeInteger digits: limbs negative: self < 0
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
