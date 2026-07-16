Integer := Number [

	"arithmetic"

	// aNumber [
		"Integer quotient truncating toward negative infinity (floored division)."
		| q |

		q := self quo: aNumber.
		((self - (q * aNumber)) = 0 or: [(self < 0) = (aNumber < 0)]) ifTrue: [^q].
		^q - 1
	]


	"mathematical functions"

	floorLog: radix [
		| me result |

		self < 0 ifTrue: ["Arithmetic"Error signal: 'cannot extract logarithm of a negative number'].
		radix <= radix unity ifTrue: [
			radix <= radix zero ifTrue: ["Arithmetic"Error signal: 'base of a logarithm cannot be negative'].
			radix = radix unity ifTrue: ["Arithmetic"Error signal: 'base of a logarithm cannot be 1'].
			^(self ceilingLog: radix reciprocal) negated
		].

		"radix isInteger ifFalse: [^(radix coerce: self) floorLog: radix]."
		me := self.
		result := 0.
		[me >= radix] whileTrue: [
			me := me // radix.
			result := result + 1.
		].

		^result
	]


	"iterators"

	timesRepeat: aBlock [
		1 to: self do: [ :each | aBlock value].
	]


	"factorization and divisibility"

	factorial [
		| result |

		self < 0 ifTrue: [^self error: 'factorial of a negative number'].
		result := 1.
		2 to: self do: [ :i | result := result * i].
		^result
	]


	gcd: anInteger [
		| a b t |

		a := self abs.
		b := anInteger abs.
		[b = 0] whileFalse: [
			t := b.
			b := a \\ b.
			a := t].
		^a
	]


	lcm: anInteger [
		(self = 0 or: [anInteger = 0]) ifTrue: [^0].
		^((self // (self gcd: anInteger)) * anInteger) abs
	]


	"testing"

	isInteger [
		^true
	]


	"converting"

	asInteger [
		^self
	]


	asScaledDecimal [
		^ScaledDecimal basicNumerator: self denominator: 1 scale: 0
	]

]
