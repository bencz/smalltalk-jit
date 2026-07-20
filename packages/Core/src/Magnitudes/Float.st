"Abstract double precision floating point number. Concrete instances are
BoxedFloat64 (heap allocated) or SmallFloat64 (immediate); the whole protocol
lives here and the subclasses only fix the representation. Instances are
created from float literals (3.14, 1.5e-3) and by #asFloat coercion."

Float := Number [

	"instance creation"

	class pi [
		^3.141592653589793
	]


	class e [
		^2.718281828459045
	]


	class infinity [
		^1.0 / 0.0
	]


	class nan [
		^0.0 / 0.0
	]


	"arithmetic"

	+ aNumber [
		<primitive: FloatAddPrimitive>
		^self retry: #+ coercing: aNumber
	]


	- aNumber [
		<primitive: FloatSubPrimitive>
		^self retry: #- coercing: aNumber
	]


	* aNumber [
		<primitive: FloatMulPrimitive>
		^self retry: #* coercing: aNumber
	]


	/ aNumber [
		<primitive: FloatDivPrimitive>
		^self retry: #/ coercing: aNumber
	]


	// aNumber [
		^(self / aNumber) floor
	]


	\\ aNumber [
		^self - ((self // aNumber) * aNumber)
	]


	negated [
		^0.0 - self
	]


	"comparing"

	< aNumber [
		<primitive: FloatLessThanPrimitive>
		^self retryRelationalOp: #< coercing: aNumber
	]


	"IEEE-754 ordering: EVERY comparison against NaN answers false. Magnitude
	derives >= as (self < x) not, which would answer true for NaN, so Float
	overrides the derived comparisons. The JIT call-site intrinsic implements
	the same semantics directly (unordered = false); these methods keep the
	dispatch (mixed-type / intrinsic-miss) path consistent with it."

	> aNumber [
		^(aNumber isKindOf: Number)
			ifTrue: [aNumber asFloat < self]
			ifFalse: [self retryRelationalOp: #> coercing: aNumber]
	]


	>= aNumber [
		^(self > aNumber) or: [self = aNumber]
	]


	<= aNumber [
		^(self < aNumber) or: [self = aNumber]
	]


	= aNumber [
		<primitive: FloatEqualsPrimitive>
		(aNumber isKindOf: Number) ifTrue: [^self retryRelationalOp: #= coercing: aNumber].
		^false
	]


	hash [
		"Value-based, so hash(2.0) = hash(2) as required by 2.0 = 2 (via
		retry:coercing:). Infinities and NaN cannot be truncated; give them a
		fixed hash (Float infinity = Float infinity holds, NaN equals nothing)."
		self isFinite ifFalse: [^0].
		^self truncated hash
	]


	"testing"

	isZero [
		^self = 0.0
	]


	isNaN [
		^self ~= self
	]


	isFinite [
		"NaN and the infinities poison subtraction: x - x is 0.0 only for
		finite x."
		^(self - self) = 0.0
	]


	isInfinite [
		^self isFinite not and: [self isNaN not]
	]


	"mathematical functions"

	sqrt [
		<primitive: FloatSqrtPrimitive>
	]


	sin [
		<primitive: FloatSinPrimitive>
	]


	cos [
		<primitive: FloatCosPrimitive>
	]


	tan [
		<primitive: FloatTanPrimitive>
	]


	arcSin [
		<primitive: FloatArcSinPrimitive>
	]


	arcCos [
		<primitive: FloatArcCosPrimitive>
	]


	arcTan [
		<primitive: FloatArcTanPrimitive>
	]


	arcTan: aNumber [
		"Two-argument arctangent: self arcTan: aNumber == atan2(self, aNumber)."
		<primitive: FloatArcTan2Primitive>
		^self arcTan: aNumber asFloat
	]


	exp [
		<primitive: FloatExpPrimitive>
	]


	ln [
		<primitive: FloatLnPrimitive>
	]


	log: aNumber [
		^self ln / aNumber ln
	]


	exponent [
		"Base-2 exponent: 1.0 exponent = 0, 0.5 exponent = -1."
		<primitive: FloatExponentPrimitive>
		^self error: 'zero, Infinity and NaN have no exponent'
	]


	timesTwoPower: anInteger [
		"self * 2^anInteger, exact (the mantissa is untouched)."
		<primitive: FloatTimesTwoPowerPrimitive>
		^self error: 'power must be a SmallInteger'
	]


	"truncation and rounding"

	truncated [
		<primitive: FloatTruncatedPrimitive>
		"The primitive only covers the SmallInteger range. Any finite double
		beyond it is already integral (|x| >= 2^53 has no fraction bits), so
		rebuild the exact integer from the 53-bit mantissa and the base-2
		exponent; the multiplication promotes to LargeInteger."
		| e m |
		self isFinite ifFalse: [^self error: 'cannot truncate Infinity or NaN'].
		e := self exponent.
		m := (self timesTwoPower: 52 - e) truncated.
		^m * (2 raisedToInteger: e - 52)
	]


	floor [
		<primitive: FloatFloorPrimitive>
		"out of the primitive's range the value is integral: floor = truncated"
		^self truncated
	]


	ceiling [
		<primitive: FloatCeilingPrimitive>
		"out of the primitive's range the value is integral: ceiling = truncated"
		^self truncated
	]


	rounded [
		<primitive: FloatRoundedPrimitive>
		"out of the primitive's range the value is integral: rounded = truncated"
		^self truncated
	]


	"coercing"

	generality [
		^500
	]


	coerce: aNumber [
		^aNumber asFloat
	]


	asFloat [
		^self
	]


	asInteger [
		^self truncated
	]


	"printing"

	printString [
		<primitive: FloatAsStringPrimitive>
	]


	printOn: aStream [
		aStream nextPutAll: self printString.
	]


	printShowingDecimalPlaces: places [
		"Answer a fixed-point String with exactly `places` fractional digits, using
		 half-up rounding and no scientific notation. Implemented over integers: the
		 magnitude is scaled by 10^places, rounded from the next digit, then the
		 decimal point is reinserted."
		| neg pow scaledX10 lastDigit scaled intPart frac fracStr |
		places < 0 ifTrue: [^self error: 'places must be non-negative'].
		self isFinite ifFalse: [^self printString].
		places = 0 ifTrue: [^self rounded printString].
		neg := self < 0.0.
		pow := 10 raisedToInteger: places.
		scaledX10 := (self abs * pow * 10) truncated.
		lastDigit := scaledX10 \\ 10.
		scaled := scaledX10 // 10.
		lastDigit >= 5 ifTrue: [scaled := scaled + 1].
		scaled = 0 ifTrue: [neg := false].
		intPart := scaled // pow.
		frac := scaled \\ pow.
		fracStr := frac printString.
		[fracStr size < places] whileTrue: [fracStr := '0', fracStr].
		^(neg ifTrue: ['-'] ifFalse: ['']), intPart printString, '.', fracStr
	]

]
