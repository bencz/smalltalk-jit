"LargeInteger arithmetic cross-checked against an INDEPENDENT reference.

 The representation moved from an Array of tagged base-2^16 limbs with the
 algorithms in Smalltalk, to a byte payload of 32-bit limbs with the algorithms
 in C (Karatsuba, Knuth Algorithm D, binary GCD, chunked radix conversion).
 Every one of those is a different algorithm, not a translation, so agreeing
 with a table of expected values proves much less than agreeing with a second
 implementation over thousands of random operand pairs.

 The reference below is the SLOW, OBVIOUS way to compute each operation, using
 only operations the change did not touch, so it cannot share a bug with the
 kernel:

   * multiply is repeated addition by doubling (shift-and-add), never *
   * divide is recovered from multiply: the quotient is found by binary search
     over the bits, using only comparison and shift-and-add
   * gcd is Euclid, from divide
   * printString is checked by round-tripping through the parser

 Operands deliberately span the SmallInteger boundary, the 32-bit limb
 boundary, and the Karatsuba threshold, in both signs, including the cases
 where one operand is a SmallInteger so the mixed-operand path in the kernel is
 exercised too.

 A deterministic generator is used rather than Random, so a failure is
 reproducible from the seed alone.

 COST SPLIT, which is why the operand sizes look modest. The reference is
 QUADRATIC or worse by construction, so running it on operands past the
 Karatsuba threshold (40 limbs, 1280 bits) costs seconds and made this the
 slowest test in the suite by an order of magnitude. It does not need to:
 ST_BIGINT_TEST (vm/tests/BigIntTest.c) already cross-checks Karatsuba against
 schoolbook, and Knuth D against the identities, over sizes up to 399 limbs, in
 C where it is cheap. THIS file's job is the OBJECT layer the C test cannot
 see: signs, normalization and demotion at the SmallInteger boundary, mixed
 SmallInteger/LargeInteger operands, coercion convergence, and printing.

 So the reference-driven checks stay at sizes where the reference is affordable,
 while the IDENTITY-driven checks (which run the fast kernel on both sides of
 the equation) deliberately go past the Karatsuba threshold."

DiffRef := Object [

	"--- a reproducible generator, independent of Random ---"

	| Seed |

	class seed: anInteger [ Seed := anInteger ]

	class next [
		"A 30-bit LCG. Kept narrow ON PURPOSE: the product must stay inside the
		 immediate range so the generator never invokes the LargeInteger code it
		 exists to test, and never needs bitAnd:, which a LargeInteger still does
		 not understand."
		Seed := (Seed * 1103515245 + 12345) \\ 1073741824.
		^Seed
	]

	class nextMagnitudeOfBits: bits [
		"A non-negative integer of roughly `bits` bits, built by shifting in
		 chunks so the value itself is produced without multiplication."
		| v |
		v := 0.
		[v highBitSafe < bits] whileTrue: [
			v := (v bitShift: 30) + self next].
		^v
	]

	"--- the reference implementations ---"

	class refMul: a with: b [
		"Shift-and-add: uses only +, bitShift: and bitAt:, never *."
		| result magA magB i |
		(a = 0 or: [b = 0]) ifTrue: [^0].
		magA := a abs.
		magB := b abs.
		result := 0.
		i := 0.
		[i < magB highBitSafe] whileTrue: [
			(magB bitAtSafe: i) = 1
				ifTrue: [result := result + (magA bitShift: i)].
			i := i + 1].
		^(a < 0) = (b < 0) ifTrue: [result] ifFalse: [0 - result]
	]

	class refQuoRem: a with: b [
		"Truncating division by binary search over the quotient's bits, using
		 only comparison, shift and the reference multiply. Answers an Array of
		 quotient and remainder, both with truncating (toward zero) signs."
		| magA magB q rem shift quotient |
		magA := a abs.
		magB := b abs.
		magA < magB ifTrue: [
			^Array with: 0 with: a].
		shift := magA highBitSafe - magB highBitSafe.
		q := 0.
		rem := magA.
		[shift >= 0] whileTrue: [
			| trial |
			trial := magB bitShift: shift.
			trial <= rem ifTrue: [
				rem := rem - trial.
				q := q + (1 bitShift: shift)].
			shift := shift - 1].
		quotient := (a < 0) = (b < 0) ifTrue: [q] ifFalse: [0 - q].
		^Array with: quotient with: (a < 0 ifTrue: [0 - rem] ifFalse: [rem])
	]

	class refGcd: a with: b [
		"Euclid on top of the reference remainder."
		| x y t |
		x := a abs.
		y := b abs.
		[y = 0] whileFalse: [
			t := y.
			y := (self refQuoRem: x with: y) at: 2.
			y := y abs.
			x := t].
		^x
	]

]

"highBit is not defined for LargeInteger (it needs bitAnd:, which a
 LargeInteger still does not understand), so the reference uses a shift-only
 bit length that works for every Integer."
Integer extend [
	bitAtSafe: index [
		"The 0-based bit, using only bitShift: and \\. bitAt: cannot be used
		 because it is built on bitAnd:, which a LargeInteger does not
		 understand: writing this reference is itself a demonstration of that
		 gap. Only ever applied to non-negative values here."
		^(self bitShift: index negated) \\ 2
	]


	highBitSafe [
		| n bits |
		self = 0 ifTrue: [^0].
		n := self abs.
		bits := 0.
		[n > 0] whileTrue: [
			n := n bitShift: -1.
			bits := bits + 1].
		^bits
	]
]

[ | t bad cases a b |
	t := TestRun named: 'LargeIntegerDifferential'.
	DiffRef seed: 20260725.

	"--- signed multiply against shift-and-add ---"
	bad := 0.
	#(40 70 130 200) do: [:bits |
		1 to: 3 do: [:iter |
			a := DiffRef nextMagnitudeOfBits: bits.
			b := DiffRef nextMagnitudeOfBits: (bits // 2) + 30.
			(Array with: 1 with: -1) do: [:sa |
				(Array with: 1 with: -1) do: [:sb | | x y |
					x := a * sa.
					y := b * sb.
					(x * y) = (DiffRef refMul: x with: y)
						ifFalse: [bad := bad + 1]]]]].
	t check: 'multiply matches shift-and-add, over sizes and both signs'
		equals: 0 is: [ bad ].

	"--- signed quo:/rem: against binary-search division ---"
	bad := 0.
	#(40 100 160) do: [:bits |
		1 to: 3 do: [:iter |
			a := DiffRef nextMagnitudeOfBits: bits.
			b := DiffRef nextMagnitudeOfBits: (bits // 3) + 20.
			b = 0 ifFalse: [
				(Array with: 1 with: -1) do: [:sa |
					(Array with: 1 with: -1) do: [:sb | | x y ref |
						x := a * sa.
						y := b * sb.
						ref := DiffRef refQuoRem: x with: y.
						(x quo: y) = (ref at: 1) ifFalse: [bad := bad + 1].
						(x rem: y) = (ref at: 2) ifFalse: [bad := bad + 1]]]]]].
	t check: 'quo: and rem: match binary-search division, both signs'
		equals: 0 is: [ bad ].

	"--- the division identities, which pin // and \\ against quo: and rem: ---"
	bad := 0.
	#(60 150 400 1400 2000) do: [:bits |
		1 to: 4 do: [:iter |
			a := DiffRef nextMagnitudeOfBits: bits.
			b := DiffRef nextMagnitudeOfBits: (bits // 3) + 20.
			b = 0 ifFalse: [
				(Array with: 1 with: -1) do: [:sa |
					(Array with: 1 with: -1) do: [:sb | | x y |
						x := a * sa.
						y := b * sb.
						"truncating: a = (a quo: b) * b + (a rem: b)"
						(((x quo: y) * y) + (x rem: y)) = x ifFalse: [bad := bad + 1].
						"floored: a = (a // b) * b + (a \\ b), and the remainder
						 takes the DIVISOR's sign"
						(((x // y) * y) + (x \\ y)) = x ifFalse: [bad := bad + 1].
						(x \\ y) = 0 ifFalse: [
							((x \\ y) < 0) = (y < 0) ifFalse: [bad := bad + 1]].
						"and rem: takes the DIVIDEND's sign"
						(x rem: y) = 0 ifFalse: [
							((x rem: y) < 0) = (x < 0) ifFalse: [bad := bad + 1]]]]]]].
	t check: 'the truncating and floored division identities hold, both signs'
		equals: 0 is: [ bad ].

	"--- gcd against Euclid ---"
	bad := 0.
	#(50 100) do: [:bits |
		1 to: 3 do: [:iter |
			a := DiffRef nextMagnitudeOfBits: bits.
			b := DiffRef nextMagnitudeOfBits: (bits // 2) + 20.
			(a = 0 or: [b = 0]) ifFalse: [
				(a gcd: b) = (DiffRef refGcd: a with: b) ifFalse: [bad := bad + 1].
				"a gcd is non-negative and divides both"
				(a gcd: b) < 0 ifTrue: [bad := bad + 1].
				(a \\ (a gcd: b)) = 0 ifFalse: [bad := bad + 1].
				(b \\ (a gcd: b)) = 0 ifFalse: [bad := bad + 1]]]].
	t check: 'gcd matches Euclid, is non-negative, and divides both operands'
		equals: 0 is: [ bad ].

	"--- add and subtract, checked by their own identities ---"
	bad := 0.
	#(40 100 300 1400 2600) do: [:bits |
		1 to: 4 do: [:iter |
			a := DiffRef nextMagnitudeOfBits: bits.
			b := DiffRef nextMagnitudeOfBits: bits.
			(Array with: 1 with: -1) do: [:sa |
				(Array with: 1 with: -1) do: [:sb | | x y |
					x := a * sa.
					y := b * sb.
					(x + y - y) = x ifFalse: [bad := bad + 1].
					(x + y) = (y + x) ifFalse: [bad := bad + 1].
					(x - y) = (0 - (y - x)) ifFalse: [bad := bad + 1].
					(x * y) = (y * x) ifFalse: [bad := bad + 1]]]]].
	t check: 'add and subtract round-trip and commute, both signs'
		equals: 0 is: [ bad ].

	"--- shifting against multiplication and floored division ---"
	bad := 0.
	#(40 100 400 1500) do: [:bits |
		1 to: 3 do: [:iter |
			a := DiffRef nextMagnitudeOfBits: bits.
			(Array with: 1 with: -1) do: [:sa | | x |
				x := a * sa.
				#(1 7 31 32 33 64 100) do: [:k |
					(x bitShift: k) = (x * (2 raisedTo: k)) ifFalse: [bad := bad + 1].
					"a right shift FLOORS, so it must equal // by the power of two"
					(x bitShift: k negated) = (x // (2 raisedTo: k))
						ifFalse: [bad := bad + 1]]]]].
	t check: 'bitShift: matches * and // by powers of two, both signs'
		equals: 0 is: [ bad ].

	"--- printing round-trips through the parser, in several radices ---"
	bad := 0.
	#(40 100 300 1400) do: [:bits |
		1 to: 3 do: [:iter |
			a := DiffRef nextMagnitudeOfBits: bits.
			(Array with: 1 with: -1) do: [:sa | | x |
				x := a * sa.
				x printString asNumber = x ifFalse: [bad := bad + 1].
				#(2 8 16 36) do: [:base | | s parsed |
					s := x abs printStringBase: base.
					parsed := LargeInteger fromString: s base: base negative: false.
					parsed = x abs ifFalse: [bad := bad + 1]]]]].
	t check: 'printString round-trips, and printStringBase: parses back'
		equals: 0 is: [ bad ].

	"--- asFloat is correctly rounded, checked where the value is exact ---"
	bad := 0.
	1 to: 40 do: [:iter | | v |
		"values below 2^53 convert exactly, so any error shows up immediately"
		v := DiffRef next bitAnd: 9007199254740991.
		v asFloat truncated = v ifFalse: [bad := bad + 1]].
	t check: 'asFloat is exact below 2^53' equals: 0 is: [ bad ].
	t check: 'asFloat of a huge value is finite and huge' equals: true
		is: [ | f | f := (2 raisedTo: 900) asFloat. f isFinite and: [f > 1.0e270] ].
	t check: 'asFloat of a power of two is exact' equals: true
		is: [ (2 raisedTo: 200) asFloat = (2.0 raisedTo: 200) ].
	t check: 'a negative LargeInteger converts with its sign' equals: true
		is: [ (0 - (2 raisedTo: 200)) asFloat < 0 ].

	"--- the SmallInteger boundary, in both directions ---"
	bad := 0.
	cases := Array with: SmallInteger maxVal with: SmallInteger minVal
		with: SmallInteger maxVal - 1 with: SmallInteger minVal + 1.
	cases do: [:v |
		(v + 1 - 1) = v ifFalse: [bad := bad + 1].
		(v - 1 + 1) = v ifFalse: [bad := bad + 1].
		(v * 2 // 2) = v ifFalse: [bad := bad + 1].
		((v + 1) - 1) class = SmallInteger ifFalse: [bad := bad + 1]].
	t check: 'values at the immediate boundary survive a round trip and demote'
		equals: 0 is: [ bad ].

	"--- mixed SmallInteger and LargeInteger operands ---"
	bad := 0.
	a := 2 raisedTo: 200.
	1 to: 50 do: [:i |
		((a + i) - i) = a ifFalse: [bad := bad + 1].
		((a * i) // i) = a ifFalse: [bad := bad + 1].
		(i + a) = (a + i) ifFalse: [bad := bad + 1].
		(i * a) = (a * i) ifFalse: [bad := bad + 1].
		(i < a) ifFalse: [bad := bad + 1].
		(a > i) ifFalse: [bad := bad + 1].
		(i = a) ifTrue: [bad := bad + 1]].
	t check: 'a SmallInteger operand mixes with a LargeInteger, both orders'
		equals: 0 is: [ bad ].

	^t report
]
