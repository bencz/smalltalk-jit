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
		"Integer quotient, truncating toward negative infinity.

		 Generic, not subclassResponsibility. These four (// \\ quo: rem:) all
		 derive from / plus a rounding method, so any pair the coercion protocol
		 lifts to a common representation can be divided. They used to raise,
		 which made `3 // 2.5` an error: SmallInteger>>// delegates to quo:, the
		 primitive fails on a Float argument, retry:coercing: lifts both to
		 Float, and Float defines no quo:, so the send landed on Number and
		 died. Only the same-representation cases were ever exercised."
		^(self / aNumber) floor
	]


	\\ aNumber [
		"Remainder of //, so it takes the sign of the DIVISOR (modulo)."
		^self - ((self // aNumber) * aNumber)
	]


	abs [
		self < 0 ifTrue: [^self negated].
		^self
	]


	negated [
		^0 - self
	]


	quo: aNumber [
		"Integer quotient, truncating toward zero. See // for why this is generic."
		^(self / aNumber) truncated
	]


	rem: aNumber [
		"Remainder of quo:, so it takes the sign of the DIVIDEND."
		^self - ((self quo: aNumber) * aNumber)
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
		"Coerce the receiver and aNumber to a common generality and retry aSymbol.

		 EVERY mixed-type arithmetic operation in the language funnels through
		 here, so its cost is the cost of `3 + 2.5`. It used to end in
		 `perform:with:`, which is Object>>perform:with: -> Behavior>>
		 lookupSelector:, a method-dictionary walk up the superclass chain
		 written in Smalltalk, plus an Array allocation for the arguments.
		 Measured, that single send was about 390 ns of a 470 ns operation:
		 mixed arithmetic cost roughly 100x same-type arithmetic, which has a
		 JIT call-site intrinsic.

		 Dispatching the common selectors directly turns that lookup into a
		 handful of identity comparisons against interned symbols, each of which
		 the JIT compiles to a bare pointer compare with no send at all. The
		 perform: tail stays so the method remains total over any selector, and
		 so a class adding its own coercible operator keeps working.

		 The cascade is ordered by how often each selector actually appears.

		 WARNING: on EQUAL generality this takes the ifFalse: branch and re-sends
		 the same selector to the same representation. That is only not an
		 infinite loop because every concrete class short-circuits its own type
		 before calling retry:, so the re-send finds a primitive rather than
		 coming back here. Any new Number subclass MUST preserve that."
		| a b |
		(aNumber isKindOf: Number) ifFalse: [^self error: aNumber printString, ' is not a Number'].
		self generality > aNumber generality
			ifTrue: [a := self. b := self coerce: aNumber]
			ifFalse: [a := aNumber coerce: self. b := aNumber].

		aSymbol == #+ ifTrue: [^a + b].
		aSymbol == #- ifTrue: [^a - b].
		aSymbol == #* ifTrue: [^a * b].
		aSymbol == #< ifTrue: [^a < b].
		aSymbol == #> ifTrue: [^a > b].
		aSymbol == #= ifTrue: [^a = b].
		aSymbol == #/ ifTrue: [^a / b].
		aSymbol == #<= ifTrue: [^a <= b].
		aSymbol == #>= ifTrue: [^a >= b].
		aSymbol == #~= ifTrue: [^a ~= b].
		aSymbol == #// ifTrue: [^a // b].
		aSymbol == #\\ ifTrue: [^a \\ b].
		aSymbol == #quo: ifTrue: [^a quo: b].
		aSymbol == #rem: ifTrue: [^a rem: b].

		^a perform: aSymbol with: b
	]


	retryRelationalOp: aSymbol coercing: aNumber [
		"Kept as a distinct selector because Float, Fraction, LargeInteger and
		 SmallInteger all name it, but it has always had a body identical to
		 retry:coercing:. Delegating rather than duplicating removes the risk of
		 the two drifting apart, which would be invisible until some operand
		 pair took the stale copy."
		^self retry: aSymbol coercing: aNumber
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
