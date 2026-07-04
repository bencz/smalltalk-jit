"Arbitrary precision integer.

The magnitude is stored in `digits`, an Array of base-2^16 limbs, least
significant first, with no trailing zero limbs (size >= 1). The sign is held
separately in `negative`. Results that fit in a SmallInteger are demoted back
to SmallInteger by #normalize, so LargeInteger only ever holds values outside
the SmallInteger range. Instances are produced automatically when SmallInteger
arithmetic overflows (see SmallInteger)."

LargeInteger := Integer [

	| digits negative |


	"--- instance creation ---"

	class digits: anArray negative: aBoolean [
		^self new setDigits: anArray negative: aBoolean
	]


	setDigits: anArray negative: aBoolean [
		digits := anArray.
		negative := aBoolean.
		^self
	]


	"--- accessing ---"

	digits [
		^digits
	]


	negative [
		^negative
	]


	isZero [
		^digits size = 1 and: [(digits at: 1) = 0]
	]


	"--- magnitude helpers (operate on normalized limb Arrays) ---"

	class normalizeArray: anArray [
		"Answer anArray without trailing zero limbs (keeping at least one limb)."
		| n |

		n := anArray size.
		[n > 1 and: [(anArray at: n) = 0]] whileTrue: [n := n - 1].
		n = anArray size ifTrue: [^anArray].
		^self copyArray: anArray size: n
	]


	class copyArray: anArray size: n [
		| r |

		r := Array new: n.
		1 to: n do: [ :i | r at: i put: (anArray at: i)].
		^r
	]


	class compareMagnitude: a and: b [
		"Answer -1, 0 or 1 comparing the two magnitudes."
		| na nb |

		na := a size.
		nb := b size.
		na < nb ifTrue: [^-1].
		na > nb ifTrue: [^1].
		na to: 1 by: -1 do: [ :i |
			(a at: i) < (b at: i) ifTrue: [^-1].
			(a at: i) > (b at: i) ifTrue: [^1]].
		^0
	]


	class addMagnitude: a and: b [
		| na nb n r carry sum |

		na := a size.
		nb := b size.
		n := na max: nb.
		r := Array new: n + 1.
		carry := 0.
		1 to: n do: [ :i |
			sum := carry
				+ (i <= na ifTrue: [a at: i] ifFalse: [0])
				+ (i <= nb ifTrue: [b at: i] ifFalse: [0]).
			r at: i put: (sum bitAnd: 65535).
			carry := sum bitShift: -16].
		r at: n + 1 put: carry.
		^self normalizeArray: r
	]


	class subtractMagnitude: a and: b [
		"Answer a - b, assuming a >= b."
		| na nb r borrow diff |

		na := a size.
		nb := b size.
		r := Array new: na.
		borrow := 0.
		1 to: na do: [ :i |
			diff := (a at: i) - borrow - (i <= nb ifTrue: [b at: i] ifFalse: [0]).
			diff < 0
				ifTrue: [diff := diff + 65536. borrow := 1]
				ifFalse: [borrow := 0].
			r at: i put: diff].
		^self normalizeArray: r
	]


	class multiplyMagnitude: a and: b [
		| na nb r carry idx t |

		na := a size.
		nb := b size.
		r := Array new: na + nb.
		1 to: na + nb do: [ :i | r at: i put: 0].
		1 to: na do: [ :i |
			carry := 0.
			1 to: nb do: [ :j |
				idx := i + j - 1.
				t := (r at: idx) + ((a at: i) * (b at: j)) + carry.
				r at: idx put: (t bitAnd: 65535).
				carry := t bitShift: -16].
			r at: i + nb put: (r at: i + nb) + carry].
		^self normalizeArray: r
	]


	class shiftLeft1Magnitude: a addBit: bit [
		"Answer a * 2 + bit."
		| n r carry v |

		n := a size.
		r := Array new: n + 1.
		carry := bit.
		1 to: n do: [ :i |
			v := ((a at: i) bitShift: 1) + carry.
			r at: i put: (v bitAnd: 65535).
			carry := v bitShift: -16].
		r at: n + 1 put: carry.
		^self normalizeArray: r
	]


	class divModMagnitude: a bySmall: d [
		"Divide magnitude a by the single limb d (1..65535).
		Answer an Array {quotientArray. remainderSmallInteger}."
		| n q rem cur |

		n := a size.
		q := Array new: n.
		rem := 0.
		n to: 1 by: -1 do: [ :i |
			cur := (rem bitShift: 16) + (a at: i).
			q at: i put: cur // d.
			rem := cur \\ d].
		^Array with: (self normalizeArray: q) with: rem
	]


	class divModMagnitude: a and: b [
		"Divide magnitude a by magnitude b (b ~= 0). Answer an Array
		{quotientArray. remainderArray} using binary long division."
		| cmp q r dm |

		cmp := self compareMagnitude: a and: b.
		cmp < 0 ifTrue: [^Array with: (Array with: 0) with: a].
		cmp = 0 ifTrue: [^Array with: (Array with: 1) with: (Array with: 0)].
		b size = 1 ifTrue: [
			dm := self divModMagnitude: a bySmall: (b at: 1).
			^Array with: (dm at: 1) with: (Array with: (dm at: 2))].

		q := Array new: a size.
		1 to: a size do: [ :i | q at: i put: 0].
		r := Array with: 0.
		a size to: 1 by: -1 do: [ :limbIndex |
			15 to: 0 by: -1 do: [ :bit |
				r := self shiftLeft1Magnitude: r
					addBit: (((a at: limbIndex) bitShift: bit negated) bitAnd: 1).
				(self compareMagnitude: r and: b) >= 0 ifTrue: [
					r := self subtractMagnitude: r and: b.
					q at: limbIndex put: ((q at: limbIndex) bitOr: (1 bitShift: bit))]]].
		^Array with: (self normalizeArray: q) with: (self normalizeArray: r)
	]


	"--- private construction ---"

	makeDigits: mag negative: aBoolean [
		"Wrap a magnitude Array + sign into a normalized number
		(a SmallInteger when it fits)."
		^(LargeInteger digits: mag negative: aBoolean) normalize
	]


	normalize [
		"Answer an equal SmallInteger when the magnitude fits in one, else self."
		| n value |

		digits := LargeInteger normalizeArray: digits.
		n := digits size.
		(n > 4 or: [n = 4 and: [(digits at: 4) >= 8192]]) ifTrue: [^self].
		value := 0.
		n to: 1 by: -1 do: [ :i | value := value * 65536 + (digits at: i)].
		^negative ifTrue: [value negated] ifFalse: [value]
	]


	"--- arithmetic ---"

	+ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^self addLarge: aNumber asLargeInteger].
		^self retry: #+ coercing: aNumber
	]


	- aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^self addLarge: aNumber negated asLargeInteger].
		^self retry: #- coercing: aNumber
	]


	* aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			| other |
			other := aNumber asLargeInteger.
			^self makeDigits: (LargeInteger multiplyMagnitude: digits and: other digits)
				negative: negative ~= other negative].
		^self retry: #* coercing: aNumber
	]


	/ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^Fraction numerator: self denominator: aNumber].
		^self retry: #/ coercing: aNumber
	]


	// aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self floorDivRem: aNumber asLargeInteger) at: 1].
		^self retry: #// coercing: aNumber
	]


	\\ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self floorDivRem: aNumber asLargeInteger) at: 2].
		^self retry: #\\ coercing: aNumber
	]


	quo: aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self divRemLarge: aNumber asLargeInteger) at: 1].
		^self retry: #quo: coercing: aNumber
	]


	rem: aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self divRemLarge: aNumber asLargeInteger) at: 2].
		^self retry: #rem: coercing: aNumber
	]


	addLarge: aLarge [
		"Signed addition of two LargeIntegers."
		| mag cmp |

		negative = aLarge negative ifTrue: [
			^self makeDigits: (LargeInteger addMagnitude: digits and: aLarge digits)
				negative: negative].

		cmp := LargeInteger compareMagnitude: digits and: aLarge digits.
		cmp = 0 ifTrue: [^0].
		cmp > 0 ifTrue: [
			^self makeDigits: (LargeInteger subtractMagnitude: digits and: aLarge digits)
				negative: negative].
		^self makeDigits: (LargeInteger subtractMagnitude: aLarge digits and: digits)
			negative: aLarge negative
	]


	divRemLarge: aLarge [
		"Truncating division. Answer {quotient. remainder}, remainder has sign of self."
		| dm q r |

		aLarge isZero ifTrue: [^self error: 'division by zero'].
		dm := LargeInteger divModMagnitude: digits and: aLarge digits.
		q := self makeDigits: (dm at: 1) negative: negative ~= aLarge negative.
		r := self makeDigits: (dm at: 2) negative: negative.
		^Array with: q with: r
	]


	floorDivRem: aLarge [
		"Floored division. Answer {quotient. remainder}, remainder has sign of divisor."
		| dr q r |

		dr := self divRemLarge: aLarge.
		q := dr at: 1.
		r := dr at: 2.
		r = 0 ifTrue: [^dr].
		(negative ~= aLarge negative) ifTrue: [^Array with: q - 1 with: r + aLarge].
		^dr
	]


	negated [
		self isZero ifTrue: [^0].
		^LargeInteger digits: digits negative: negative not
	]


	abs [
		^negative ifTrue: [self negated] ifFalse: [self]
	]


	sign [
		self isZero ifTrue: [^0].
		^negative ifTrue: [-1] ifFalse: [1]
	]


	"--- comparing ---"

	compareLarge: aLarge [
		| c |

		negative ~= aLarge negative ifTrue: [^negative ifTrue: [-1] ifFalse: [1]].
		c := LargeInteger compareMagnitude: digits and: aLarge digits.
		^negative ifTrue: [c negated] ifFalse: [c]
	]


	< aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self compareLarge: aNumber asLargeInteger) < 0].
		^self retryRelationalOp: #< coercing: aNumber
	]


	= aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self compareLarge: aNumber asLargeInteger) = 0].
		(aNumber isKindOf: Number) ifTrue: [^self retryRelationalOp: #= coercing: aNumber].
		^false
	]


	hash [
		| h |

		h := 0.
		digits do: [ :d | h := (h * 65536 + d) bitAnd: 16r3FFFFFFF].
		^h
	]


	"--- testing ---"

	even [
		^((digits at: 1) bitAnd: 1) = 0
	]


	odd [
		^((digits at: 1) bitAnd: 1) = 1
	]


	"--- coercing ---"

	generality [
		^300
	]


	coerce: aNumber [
		^aNumber asLargeInteger
	]


	asLargeInteger [
		^self
	]


	asInteger [
		^self
	]


	asFloat [
		| r |

		r := 0.0.
		digits size to: 1 by: -1 do: [ :i | r := (r * 65536.0) + (digits at: i) asFloat].
		^negative ifTrue: [r negated] ifFalse: [r]
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


	unity [
		^1
	]


	zero [
		^0
	]


	"--- printing ---"

	printOn: aStream [
		| groups mag first gs |

		self isZero ifTrue: [^aStream nextPutAll: '0'].
		negative ifTrue: [aStream nextPut: $-].

		groups := OrderedCollection new.
		mag := digits.
		[mag size = 1 and: [(mag at: 1) = 0]] whileFalse: [
			| dm |
			dm := LargeInteger divModMagnitude: mag bySmall: 10000.
			groups add: (dm at: 2).
			mag := dm at: 1].

		first := true.
		[groups isEmpty] whileFalse: [
			gs := groups removeLast printString.
			first
				ifTrue: [first := false]
				ifFalse: [[gs size < 4] whileTrue: [gs := '0', gs]].
			aStream nextPutAll: gs]
	]

]
