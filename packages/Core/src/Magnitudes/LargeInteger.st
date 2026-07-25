"Arbitrary-precision integer.

The MAGNITUDE lives directly in a byte payload as 32-bit limbs, least
significant first, in native order; the SIGN is the concrete class
(LargePositiveInteger / LargeNegativeInteger), Pharo style, so there is no sign
field that could disagree with the value. The arithmetic itself is in C
(vm/runtime/BigInt.c) and this class is the thin layer that decides signs,
coerces mixed operands, and raises.

That replaced an Array of TAGGED SmallInteger limbs in base 2^16, which cost 8
bytes of GC-scanned heap per 16 bits of value and carried schoolbook multiply,
BIT BY BIT long division (a fresh limb Array allocated per bit), Euclid gcd
through \\, and O(n^2) printing. The C layer has Karatsuba, Knuth Algorithm D,
binary GCD, and chunked radix conversion.

Because the payload is byte-shaped, the snapshot and actor message passing
round-trip it verbatim with no work here.

NORMALIZATION happens in C on every result: a magnitude that fits the 62-bit
signed payload comes back as a SmallInteger instead, so a LargeInteger instance
is ALWAYS genuinely too big to be immediate. The threshold is asymmetric
([-2^61, 2^61-1]) and is shared with SmallInteger class maxVal/minVal, tagInt's
assertion, and the parser's literal builder.

WARNING: bitAnd:, bitOr: and bitXor: are still not understood by a
LargeInteger, exactly as before this rewrite. They need two's-complement
semantics over a sign-magnitude representation, which is a separate piece of
work; nothing here regressed."

LargeInteger := Integer [

	<shape: BytesShape>

	| Classes |


	"--- class side ---"

	class initialize [
		"The concrete classes are handed to every primitive as a pair rather
		 than looked up in C: adding VM Handles entries is append-only and bumps
		 the snapshot format, and a by-name global lookup per operation would be
		 a hash probe on the hot path."
		Classes := Array with: LargePositiveInteger with: LargeNegativeInteger
	]


	class classes [
		^Classes
	]


	class fromString: aString base: anInteger negative: aBoolean [
		"Digits only, no sign character. The primitive's receiver has to BE the
		 digit buffer, so the work is on String and this is the readable entry
		 point that supplies the classes pair."
		^aString largeIntegerBase: anInteger negative: aBoolean classes: Classes
	]


	"--- arithmetic ---
	 Opcodes match the enum in vm/runtime/Primitives.c; they are part of the
	 primitive's contract and must not be renumbered."

	+ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^self largeOp: 1 with: aNumber].
		^self retry: #+ coercing: aNumber
	]


	- aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^self largeOp: 2 with: aNumber].
		^self retry: #- coercing: aNumber
	]


	* aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^self largeOp: 3 with: aNumber].
		^self retry: #* coercing: aNumber
	]


	/ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^Fraction numerator: self denominator: aNumber].
		^self retry: #/ coercing: aNumber
	]


	// aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^ZeroDivide new signal].
			^self largeOp: 6 with: aNumber].
		^self retry: #// coercing: aNumber
	]


	\\ aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^ZeroDivide new signal].
			^self largeOp: 7 with: aNumber].
		^self retry: #\\ coercing: aNumber
	]


	quo: aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^ZeroDivide new signal].
			^self largeOp: 4 with: aNumber].
		^self retry: #quo: coercing: aNumber
	]


	rem: aNumber [
		(aNumber isKindOf: Integer) ifTrue: [
			aNumber = 0 ifTrue: [^ZeroDivide new signal].
			^self largeOp: 5 with: aNumber].
		^self retry: #rem: coercing: aNumber
	]


	gcd: aNumber [
		"Binary GCD in C. Integer>>gcd: is Euclid through \\, which paid a full
		 division per step."
		(aNumber isKindOf: Integer) ifTrue: [^self largeOp: 8 with: aNumber].
		^super gcd: aNumber
	]


	negated [
		^0 - self
	]


	abs [
		^self negative ifTrue: [self negated] ifFalse: [self]
	]


	sign [
		^self negative ifTrue: [-1] ifFalse: [1]
	]


	"--- bit operations ---"

	bitShift: anInteger [
		"Arithmetic shift: negative right, positive left. The C side floors a
		 right shift of a negative receiver, which sign-magnitude does not do for
		 free (shifting the magnitude truncates toward zero)."
		(anInteger isKindOf: Integer) ifFalse: [
			^InvalidArgumentError signal: 'shift count must be an Integer'].
		^self largeShift: anInteger
	]


	"--- comparing ---"

	compareLarge: aNumber [
		"-1, 0 or 1. Accepts a SmallInteger too, so no operand has to be widened
		 before comparing."
		^self largeCompare: aNumber
	]


	< aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self compareLarge: aNumber) < 0].
		^self retryRelationalOp: #< coercing: aNumber
	]


	> aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self compareLarge: aNumber) > 0].
		^self retryRelationalOp: #> coercing: aNumber
	]


	<= aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self compareLarge: aNumber) <= 0].
		^self retryRelationalOp: #<= coercing: aNumber
	]


	>= aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self compareLarge: aNumber) >= 0].
		^self retryRelationalOp: #>= coercing: aNumber
	]


	= aNumber [
		(aNumber isKindOf: Integer) ifTrue: [^(self compareLarge: aNumber) = 0].
		(aNumber isKindOf: Number) ifTrue: [^self retryRelationalOp: #= coercing: aNumber].
		^false
	]


	hash [
		"Must agree with SmallInteger>>hash wherever the values are equal, but a
		 LargeInteger is never equal to a SmallInteger by construction (it would
		 have normalized), so folding the limbs is enough."
		| h |
		h := 0.
		1 to: self basicSize do: [:i | h := (h * 257 + (self basicAt: i)) bitAnd: 16r3FFFFFFF].
		^h
	]


	"--- testing ---"

	negative [
		"The concrete classes answer their sign."
		^SubClassResponsibility signal
	]


	even [
		"Deliberately NOT a direct read of the payload's first byte. Limbs are
		 stored in NATIVE order, so byte 1 is the low byte of limb 0 on a
		 little-endian target and its HIGH byte on a big-endian one; reading it
		 would answer correctly on x86 and wrongly on POWER. Ask the arithmetic."
		^(self \\ 2) = 0
	]


	odd [
		^(self \\ 2) = 1
	]


	isZero [
		"A normalized LargeInteger is never zero: zero fits a SmallInteger."
		^false
	]


	"--- coercing ---"

	generality [
		^300
	]


	coerce: aNumber [
		"Answers the number unchanged. The only class of lower generality is
		 SmallInteger, and every kernel here takes a SmallInteger operand
		 directly, so there is nothing to widen.

		 WARNING: this means coerce: does NOT raise generality, so it cannot be
		 what makes retry:coercing: converge. Convergence comes from every
		 arithmetic and relational method above short-circuiting an Integer
		 argument BEFORE calling retry:. Breaking that short-circuit turns
		 `1 * (2 raisedTo: 64)` into infinite recursion, which is exactly how
		 this was found."
		^aNumber
	]


	asLargeInteger [
		^self
	]


	asInteger [
		^self
	]


	asFloat [
		"Correctly rounded (round half to even) in C. The old version was a
		 Horner loop in doubles, which accumulates error and silently produced
		 Infinity above 2^1024 instead of answering it deliberately."
		^self primAsFloatWithClasses: LargeInteger classes
	]


	primAsFloatWithClasses: classPair [
		<primitive: LargeIntAsFloatPrimitive>
		^self error: 'LargeInteger asFloat failed'
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
		^self printOn: aStream base: 10
	]


	printOn: aStream base: anInteger [
		self negative ifTrue: [aStream nextPut: $-].
		aStream nextPutAll: (self digitsStringBase: anInteger)
	]


	printStringBase: anInteger [
		^self negative
			ifTrue: ['-', (self digitsStringBase: anInteger)]
			ifFalse: [self digitsStringBase: anInteger]
	]


	digitsStringBase: anInteger [
		"Digits WITHOUT a sign. Chunked in C: one division yields many digits,
		 where the image divided by 10000 at a time through a general division."
		<primitive: LargeIntPrintStringPrimitive>
		^self error: 'LargeInteger printing failed'
	]

]
