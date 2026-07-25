"LargeInteger arithmetic: multiplication, division, gcd, and radix printing.

 Every one of these is asymptotically limited by the representation, so this
 benchmark exists to make the representation's cost visible:

   * multiply is schoolbook O(n*m), with no Karatsuba;
   * divide is BIT BY BIT long division that allocates a fresh limb Array per
     bit, so a single big division is O(bits) allocations;
   * gcd: is Euclid through \\, so every step pays a full division;
   * printString repeatedly divides by 10000, which is O(n^2) in limbs.

 The workload is small enough to finish quickly on the slow representation and
 large enough that an asymptotically better one shows up clearly.

 Self-verifying: every result is checked against a value derived a DIFFERENT
 way (an identity that must hold rather than a constant copied from a previous
 run), so a printed figure means the arithmetic was right.

 Original to this repository; carries the repository's own licence, unlike
 Richards and DeltaBlue."

BigIntBenchmark := Object [

	class factorialOf: n [
		| product |
		product := 1.
		2 to: n do: [:i | product := product * i].
		^product
	]

	class run [
		| f g q r digits shifted |

		"multiply: 400! is about 869 decimal digits"
		f := self factorialOf: 400.

		"divide: exact, so the quotient identity is checkable"
		g := self factorialOf: 399.
		q := f // g.
		q = 400 ifFalse: [^self error: 'BigIntBench: 400!//399! is not 400'].

		"a non-exact division, checked by the division identity"
		r := f \\ 100003.
		((f // 100003) * 100003 + r) = f
			ifFalse: [^self error: 'BigIntBench: division identity failed'].

		"gcd: of two factorials is the smaller one"
		(f gcd: g) = g ifFalse: [^self error: 'BigIntBench: gcd is wrong'].

		"radix printing, checked by round-tripping back through the parser"
		digits := f printString.
		digits asNumber = f
			ifFalse: [^self error: 'BigIntBench: printString does not round-trip'].

		"shifting, checked against multiplication by a power of two"
		shifted := f bitShift: 64.
		shifted = (f * (2 raisedTo: 64))
			ifFalse: [^self error: 'BigIntBench: bitShift: disagrees with *'].
		(shifted bitShift: -64) = f
			ifFalse: [^self error: 'BigIntBench: bitShift: does not round-trip'].

		^digits size
	]

	class standardBenchmark [
		| watch size |
		watch := Stopwatch new.
		watch start.
		size := self run.
		watch stop.
		size = 869 ifFalse: [
			^self error: 'BigIntBench: 400! has ', size printString, ' digits, expected 869'].
		Transcript
			nextPutAll: 'BigInt ', watch elapsedMilliseconds printString, ' milliseconds';
			lf
	]

]

[
	BigIntBenchmark standardBenchmark.
]
