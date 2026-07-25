Number := Magnitude [

	"instance creation"

	class readFrom: aStream base: anInteger [
		| char sign int exp isFloat |

		isFloat := false.
		sign := 1.
		int := 0.

		char := aStream peek.
		char isNil ifTrue: [^0].
		char = $- ifTrue: [
			sign := -1.
			aStream next].

		char := aStream peek.
		char isNil ifTrue: [^0].
		char := char asUppercase.
		((char isDigit: anInteger) or: [char = $.]) ifFalse: [^0].

		[char notNil and: [(char := char asUppercase) isDigit: anInteger]] whileTrue: [
			aStream next.
			int := sign * char digitValue + (int * anInteger).
			char := aStream peek.
		].
		char isNil ifTrue: [^int].

		char = $. ifTrue: [
			aStream next.
			isFloat := true.
			[(char := aStream peek) notNil and: [(char := char asUppercase) isDigit: anInteger]] whileTrue: [
				sign := sign / anInteger.
				int := sign * char digitValue + int.
				aStream next.
			].
		].

		exp := 0.
		(anInteger = 10 and: [char = $E]) ifFalse: [
			^isFloat ifTrue: [int asFloat] ifFalse: [int]
		].

		aStream next.
		char := aStream peek.
		char isNil ifTrue: [^int].
		sign := 1.
		char = $+ ifTrue: [sign := 1. aStream next].
		char = $- ifTrue: [sign := -1. aStream next].

		[(char := aStream peek) notNil and: [char isDigit]] whileTrue: [
			exp := char digitValue + (exp * 10).
			aStream next.
		].

		int := int * (10 raisedToInteger: exp * sign).
		^int asFloat
	]


	class readFrom: aStream [
		^self readFrom: aStream base: 10
	]


	"arithmetic"

	+ aNumber [
		SubClassResponsibility signal.
	]


	- aNumber [
		SubClassResponsibility signal.
	]


	* aNumber [
		SubClassResponsibility signal.
	]


	/ aNumber [
		SubClassResponsibility signal.
	]


	// aNumber [
		"Answer the integer quotient defined by division with truncation toward negative infinity."
		SubClassResponsibility signal.
	]


	\\ aNumber [
		"Answer the integer remainder defined by division with truncation toward negative infinity.
		This is the modulo operation."
		SubClassResponsibility signal.
	]


	abs [
		self < 0 ifTrue: [^self negated].
		^self
	]


	negated [
		^0 - self
	]


	quo: aNumber [
		"Answer the integer quotient defined by division with truncation toward zero."
		SubClassResponsibility signal.
	]

	rem: aNumber [
		"Answer the integer remainder defined by division with truncation toward zero."
		SubClassResponsibility signal.
	]


	reciprocal [
		^self unity / self
	]


	"coercing (generality based, Blue Book style)"

	generality [
		"Answer a relative ranking used to coerce mixed-type arithmetic to a
		common type. Higher generality wins: SmallInteger 100 < LargeInteger 300
		< Fraction 400 < Float 500."
		SubClassResponsibility signal.
	]


	coerce: aNumber [
		"Answer aNumber converted to the receiver's own class."
		SubClassResponsibility signal.
	]


	unity [
		^1
	]


	zero [
		^0
	]


	retry: aSymbol coercing: aNumber [
		"Coerce the receiver and aNumber to a common generality and retry aSymbol."
		(aNumber isKindOf: Number) ifFalse: [^self error: aNumber printString, ' is not a Number'].
		^self generality > aNumber generality
			ifTrue: [self perform: aSymbol with: (self coerce: aNumber)]
			ifFalse: [(aNumber coerce: self) perform: aSymbol with: aNumber]
	]


	retryRelationalOp: aSymbol coercing: aNumber [
		(aNumber isKindOf: Number) ifFalse: [^self error: aNumber printString, ' is not a Number'].
		^self generality > aNumber generality
			ifTrue: [self perform: aSymbol with: (self coerce: aNumber)]
			ifFalse: [(aNumber coerce: self) perform: aSymbol with: aNumber]
	]


	"mathematical functions"

	exp [
		^self asFloat exp
	]


	ln [
		^self asFloat ln
	]


	sin [
		^self asFloat sin
	]


	cos [
		^self asFloat cos
	]


	tan [
		^self asFloat tan
	]


	arcSin [
		^self asFloat arcSin
	]


	arcCos [
		^self asFloat arcCos
	]


	arcTan [
		^self asFloat arcTan
	]


	arcTan: aNumber [
		^self asFloat arcTan: aNumber asFloat
	]


	log: aNumber [
		^self ln / aNumber ln
	]


	floorLog: aNumber [
		^(self log: aNumber) floor
	]


	ceilingLog: aNumber [
		"Companion of floorLog:. Integer>>floorLog: has always CALLED this for a
		 radix below 1 (rewriting log_r(x) as -log_1/r(x)), but it did not exist,
		 so that branch was a live doesNotUnderstand."
		^(self log: aNumber) ceiling
	]


	raisedTo: aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^self raisedToInteger: aNumber].
		^(self ln * aNumber) exp
	]


	raisedToInteger: anInteger [
		"Fast exponentiation by squaring."
		| result base exp |

		anInteger = 0 ifTrue: [^self unity].
		anInteger < 0 ifTrue: [^(self raisedToInteger: anInteger negated) reciprocal].

		result := self unity.
		base := self.
		exp := anInteger.
		[exp > 0] whileTrue: [
			(exp bitAnd: 1) = 1 ifTrue: [result := result * base].
			exp := exp bitShift: -1.
			exp > 0 ifTrue: [base := base * base]].
		^result
	]


	sqrt [
		^self asFloat sqrt
	]


	squared [
		^self * self
	]


	"testing"

	isNumber [
		^true
	]


	even [
		"Generic over the whole tower: a Fraction is never even, and a Float is
		 even exactly when its remainder mod 2 vanishes. Integer subclasses
		 override with a cheap bit test. This used to have an EMPTY body, so
		 `3.0 even` silently answered 3.0."
		^(self \\ 2) = 0
	]


	odd [
		"Deliberately NOT `self even not`. Parity is defined only for integers,
		 and `even not` would report every Fraction as odd, which is exactly the
		 kind of confident wrong answer this file is being cleaned of. Asking
		 the remainder directly makes a non-integer answer false to BOTH, so
		 `x even or: [x odd]` correctly means `x is an integer value`. Integer
		 subclasses override with a bit test, where the two are complementary."
		^(self \\ 2) = 1
	]


	negative [
		^self < 0
	]


	positive [
		^self >= 0
	]


	strictlyPositive [
		^self > 0
	]


	sign [
		self < 0 ifTrue: [^-1].
		self > 0 ifTrue: [^1].
		^0
	]


	clampBetween: aMinimum and: aMaximum [
		"Answer the receiver limited to the closed range [aMinimum, aMaximum]."
		^(self max: aMinimum) min: aMaximum
	]


	"truncation and round off"

	ceiling [
		"Smallest integer not less than the receiver, derived from truncated
		 (which rounds toward zero): only a positive fractional part moves it up."
		| truncation |
		truncation := self truncated.
		"Receiver on the LEFT so the comparison dispatches to the subclass's own
		 <, not to Integer's, which would drag the whole generality/coerce:
		 machinery in just to compare against an Integer."
		^self > truncation ifTrue: [truncation + 1] ifFalse: [truncation]
	]


	floor [
		"Largest integer not greater than the receiver, derived from truncated:
		 only a negative fractional part moves it down."
		| truncation |
		truncation := self truncated.
		^self < truncation ifTrue: [truncation - 1] ifFalse: [truncation]
	]


	truncated [
		"The only genuinely representation-specific member of this group: every
		 other rounding here derives from it. A subclass that defines nothing
		 must fail LOUDLY, not answer the receiver, which is what the former
		 empty body did."
		^SubClassResponsibility signal
	]


	truncateTo: aNumber [
		^(self / aNumber) truncated * aNumber
	]


	rounded [
		"Nearest integer, ties away from zero, matching the C round() behind
		 Float>>rounded so both paths agree on x.5."
		^self negative
			ifTrue: [(self - (1 / 2)) ceiling]
			ifFalse: [(self + (1 / 2)) floor]
	]


	integerPart [
		"The receiver with its fractional part removed (truncated toward zero)."
		^self truncated
	]


	fractionPart [
		"The receiver minus its integerPart."
		^self - self integerPart
	]


	roundTo: aNumber [
		^(self / aNumber) rounded * aNumber
	]


	"converting"

	degreesToRadians [
		^self asFloat * Float pi / 180.0
	]


	radiansToDegrees [
		^self asFloat * 180.0 / Float pi
	]


	asString [
		^self printString
	]


	"intervals"

	to: stop [
		^Interval from: self to: stop
	]


	to: stop by: step [
		^Interval from: self to: stop by: step
	]


	to: stop by: step do: aBlock [
		| i |

		i := self.
		step > step zero
			ifTrue: [
				[i <= stop] whileTrue: [
					aBlock value: i.
					i := i + step]]
			ifFalse: [
				[i >= stop] whileTrue: [
					aBlock value: i.
					i := i + step]].
	]


	to: stop do: aBlock [
		| i |

		i := self.
		[i <= stop] whileTrue: [
			aBlock value: i.
			i := i + self unity].
	]

]
