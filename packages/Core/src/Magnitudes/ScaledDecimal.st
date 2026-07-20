"Exact decimal number. The VALUE is an exact rational (numerator over
denominator; literals store the numerator over a power of ten, unreduced) and
`scale` only controls printing, which rounds half away from zero to exactly
that many decimal places. Produced by literals with the s suffix (3.14s2,
2.5s with the scale inferred, 3s0) and by arithmetic with Integers, Fractions
and other ScaledDecimals, which stays exact: 0.1s1 + 0.2s1 = 0.3s1 with no
binary float rounding involved; the result scale is the largest operand
scale. Mixing with a Float coerces to Float (generality 450 sits between
Fraction 400 and Float 500)."

ScaledDecimal := Number [

	| numerator denominator scale |


	"--- instance creation ---"

	class basicNumerator: n denominator: d scale: s [
		^self new setNumerator: n denominator: d scale: s
	]


	class number: aRational scale: s [
		"Wrap an exact Integer or reduced Fraction result."
		(aRational isKindOf: Fraction) ifTrue: [
			^self basicNumerator: aRational numerator denominator: aRational denominator scale: s].
		^self basicNumerator: aRational denominator: 1 scale: s
	]


	setNumerator: n denominator: d scale: s [
		numerator := n.
		denominator := d.
		scale := s.
		^self
	]


	"--- accessing ---"

	numerator [
		^numerator
	]


	denominator [
		^denominator
	]


	scale [
		^scale
	]


	isZero [
		^numerator = 0
	]


	"--- the exact value, and the scale of a mixed operation ---"

	exactValue [
		"The receiver's value as a plain Integer or reduced Fraction."
		^Fraction numerator: numerator denominator: denominator
	]


	exactValueOf: aNumber [
		(aNumber isKindOf: ScaledDecimal) ifTrue: [^aNumber exactValue].
		^aNumber
	]


	scaleFor: aNumber [
		"The result scale for an exact operation, or nil when the operand
		forces a coercion (Float)."
		(aNumber isKindOf: ScaledDecimal) ifTrue: [^scale max: aNumber scale].
		(aNumber isKindOf: Integer) ifTrue: [^scale].
		(aNumber isKindOf: Fraction) ifTrue: [^scale].
		^nil
	]


	"--- arithmetic (exact within the rational tower, coerced against Float) ---"

	+ aNumber [
		| s |
		s := self scaleFor: aNumber.
		s isNil ifTrue: [^self retry: #+ coercing: aNumber].
		^ScaledDecimal number: self exactValue + (self exactValueOf: aNumber) scale: s
	]


	- aNumber [
		| s |
		s := self scaleFor: aNumber.
		s isNil ifTrue: [^self retry: #- coercing: aNumber].
		^ScaledDecimal number: self exactValue - (self exactValueOf: aNumber) scale: s
	]


	* aNumber [
		| s |
		s := self scaleFor: aNumber.
		s isNil ifTrue: [^self retry: #* coercing: aNumber].
		^ScaledDecimal number: self exactValue * (self exactValueOf: aNumber) scale: s
	]


	/ aNumber [
		| s |
		s := self scaleFor: aNumber.
		s isNil ifTrue: [^self retry: #/ coercing: aNumber].
		^ScaledDecimal number: self exactValue / (self exactValueOf: aNumber) scale: s
	]


	// aNumber [
		^(self / aNumber) floor
	]


	\\ aNumber [
		^self - ((self // aNumber) * aNumber)
	]


	negated [
		^ScaledDecimal basicNumerator: numerator negated denominator: denominator scale: scale
	]


	abs [
		^numerator < 0 ifTrue: [self negated] ifFalse: [self]
	]


	sign [
		numerator = 0 ifTrue: [^0].
		^numerator < 0 ifTrue: [-1] ifFalse: [1]
	]


	"--- comparing ---"

	< aNumber [
		(self scaleFor: aNumber) isNil ifTrue: [^self retryRelationalOp: #< coercing: aNumber].
		^self exactValue < (self exactValueOf: aNumber)
	]


	= aNumber [
		(aNumber isKindOf: Number) ifFalse: [^false].
		(self scaleFor: aNumber) isNil ifTrue: [^self retryRelationalOp: #= coercing: aNumber].
		^self exactValue = (self exactValueOf: aNumber)
	]


	hash [
		"Value-based through the reduced exact value, so it agrees with the
		Integer or Fraction the receiver equals."
		^self exactValue hash
	]


	"--- coercing ---"

	generality [
		^450
	]


	coerce: aNumber [
		^aNumber asScaledDecimal
	]


	asScaledDecimal [
		^self
	]


	asFraction [
		^self exactValue
	]


	asFloat [
		^numerator asFloat / denominator asFloat
	]


	asInteger [
		^self truncated
	]


	"--- truncation and rounding (of the exact value) ---"

	truncated [
		^numerator quo: denominator
	]


	floor [
		^numerator // denominator
	]


	ceiling [
		^self negated floor negated
	]


	rounded [
		^(self exactValue + (Fraction basicNumerator: 1 denominator: 2)) floor
	]


	"--- printing ---"

	printOn: aStream [
		"Exactly `scale` decimal places, rounded half away from zero, with the
		s suffix: 3.14s2, 3.00s2, -0.7s1, 42s0."
		| p q whole frac fs |
		numerator < 0 ifTrue: [aStream nextPut: $-].
		p := 10 raisedToInteger: scale.
		q := ((numerator abs * p * 2) + denominator) // (denominator * 2).
		whole := q // p.
		aStream nextPutAll: whole printString.
		scale > 0 ifTrue: [
			frac := q \\ p.
			fs := frac printString.
			[fs size < scale] whileTrue: [fs := '0', fs].
			aStream nextPut: $.; nextPutAll: fs].
		aStream nextPut: $s.
		aStream nextPutAll: scale printString
	]

]
