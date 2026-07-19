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


	"printing"

	printStringBase: base [
		"Answer the receiver's digits in the given base (2..36) as a String. This
		 is generic over every integer, so LargeInteger works too (e.g.
		 (2 raisedTo: 100) printStringBase: 16); SmallInteger keeps its own
		 floorLog:-based fast path."
		| n neg pieces s |
		self = 0 ifTrue: [^'0'].
		neg := self < 0.
		n := self abs.
		pieces := OrderedCollection new.
		[n > 0] whileTrue: [
			pieces addFirst: (Character digitValue: n \\ base).
			n := n // base].
		neg ifTrue: [pieces addFirst: $-].
		s := String new: pieces size.
		pieces keysAndValuesDo: [ :i :c | s at: i put: c].
		^s
	]


	printString: base [
		^self printStringBase: base
	]


	printPaddedWith: aCharacter to: aWidth [
		"Answer the decimal printString left-padded with aCharacter to at least
		 aWidth characters."
		| s pad result |
		s := self printString.
		s size >= aWidth ifTrue: [^s].
		pad := aWidth - s size.
		result := String new: aWidth.
		1 to: pad do: [ :i | result at: i put: aCharacter].
		result replaceFrom: pad + 1 to: aWidth with: s startingAt: 1.
		^result
	]


	"number theory and bit access"

	sqrtFloor [
		"Integer floor of the square root, exact for LargeInteger too (integer
		 Newton iteration, no Float rounding)."
		| x y |
		self < 0 ifTrue: [^self error: 'sqrtFloor of a negative number'].
		self < 2 ifTrue: [^self].
		x := self.
		y := (x + 1) // 2.
		[y < x] whileTrue: [
			x := y.
			y := (x + (self // x)) // 2].
		^x
	]


	isPrime [
		"Trial division by 2 and the odd numbers up to sqrtFloor."
		| limit i |
		self < 2 ifTrue: [^false].
		self = 2 ifTrue: [^true].
		self even ifTrue: [^false].
		limit := self sqrtFloor.
		i := 3.
		[i <= limit] whileTrue: [
			(self \\ i) = 0 ifTrue: [^false].
			i := i + 2].
		^true
	]


	bitAt: index [
		"Answer the bit (0 or 1) at position index, counting from 1 at the least
		 significant bit."
		^(self bitShift: 1 - index) bitAnd: 1
	]


	setBit: index [
		"Answer the receiver with the 1-based bit at index set to 1."
		^self bitOr: (1 bitShift: index - 1)
	]


	clearBit: index [
		"Answer the receiver with the 1-based bit at index cleared to 0."
		^self bitAnd: (1 bitShift: index - 1) bitInvert
	]


	lowBit [
		"1-based index of the lowest set bit, 0 when the receiver is 0."
		| i n |
		self = 0 ifTrue: [^0].
		i := 1.
		n := self.
		[(n bitAnd: 1) = 0] whileTrue: [
			n := n bitShift: -1.
			i := i + 1].
		^i
	]


	bitCount [
		"Number of 1 bits (population count) of a non-negative integer."
		| n count |
		self < 0 ifTrue: [^self error: 'bitCount of a negative number'].
		n := self.
		count := 0.
		[n > 0] whileTrue: [
			count := count + (n bitAnd: 1).
			n := n bitShift: -1].
		^count
	]

]
