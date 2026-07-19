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

	even [

	]


	odd [

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

	]


	floor [

	]


	truncated [

	]


	truncateTo: aNumber [
		^(self / aNumber) truncated * aNumber
	]


	rounded [

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
