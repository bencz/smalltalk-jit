"Cross-representation Float arithmetic: every operand combination of
SmallFloat64 (immediate) and BoxedFloat64 (heap box), and every TRANSITION
between the two representations that arithmetic can cause. The representation
must be invisible in the VALUES (pure IEEE-754) while the CLASS tracks the
magnitude: immediates cover 2^-255 < |x| < 2^257 plus +-0.0, everything else
boxes. Exact powers of two are used throughout so every expected value is
bit-exact, never approximate. Fails-counter pattern (a bare Assert failure
exits 0); one line is printed per failing check."

FloatCrossRepTest := Object [

	class check: name is: ok [
		ok ifTrue: [^0].
		Transcript nextPutAll: 'FAIL: ', name; lf.
		^1
	]


	class giant [
		"a boxed float far beyond the immediate range"
		^1.0 timesTwoPower: 400
	]


	class tiny [
		"a boxed NORMAL float far below the immediate range (not subnormal)"
		^1.0 timesTwoPower: -400
	]


	"---- the full receiver x argument representation matrix, one op at a time"

	class additionMatrix [
		| f |
		f := 0.
		f := f + (self check: 'imm + imm -> imm' is: (2.5 + 0.5) = 3.0).
		f := f + (self check: 'imm + imm -> imm class' is: (2.5 + 0.5) class == SmallFloat64).
		f := f + (self check: 'imm + boxed -> boxed (absorption)' is: (2.5 + self giant) = self giant).
		f := f + (self check: 'imm + boxed class' is: (2.5 + self giant) class == BoxedFloat64).
		f := f + (self check: 'boxed + imm -> boxed (absorption)' is: (self giant + 2.5) = self giant).
		f := f + (self check: 'boxed + imm class' is: (self giant + 2.5) class == BoxedFloat64).
		f := f + (self check: 'boxed + boxed -> boxed' is: (self giant + self giant) = (1.0 timesTwoPower: 401)).
		f := f + (self check: 'boxed + boxed class' is: (self giant + self giant) class == BoxedFloat64).
		f := f + (self check: 'imm + imm -> BOXED (grows out of range)'
			is: ((1.0 timesTwoPower: 256) + (1.0 timesTwoPower: 256)) = (1.0 timesTwoPower: 257)).
		f := f + (self check: 'imm + imm -> boxed class'
			is: ((1.0 timesTwoPower: 256) + (1.0 timesTwoPower: 256)) class == BoxedFloat64).
		^f
	]


	class subtractionMatrix [
		| f |
		f := 0.
		f := f + (self check: 'boxed - boxed -> IMM (total cancellation)'
			is: (self giant - self giant) = 0.0).
		f := f + (self check: 'boxed - boxed cancellation class'
			is: (self giant - self giant) class == SmallFloat64).
		f := f + (self check: 'boxed - boxed -> boxed'
			is: ((1.0 timesTwoPower: 401) - self giant) = self giant).
		f := f + (self check: 'boxed - imm -> boxed (absorption)'
			is: (self giant - 2.5) = self giant).
		f := f + (self check: 'imm - boxed -> boxed negative'
			is: (2.5 - self giant) = (0.0 - self giant)).
		f := f + (self check: 'imm - imm -> imm' is: (3.0 - 0.5) = 2.5).
		f := f + (self check: 'negative giant cancels back to imm zero'
			is: ((0.0 - self giant) + self giant) = 0.0).
		f := f + (self check: 'cancellation zero is immediate'
			is: ((0.0 - self giant) + self giant) class == SmallFloat64).
		"partial cancellation: 2^210 sits within the 52-bit mantissa reach of
		2^258 (difference of exponents 48), so the sum is exact and the
		subtraction recovers 2^210 exactly, dropping from boxed to immediate"
		f := f + (self check: 'boxed - boxed -> imm (partial cancellation)'
			is: (((1.0 timesTwoPower: 258) + (1.0 timesTwoPower: 210)) - (1.0 timesTwoPower: 258))
				= (1.0 timesTwoPower: 210)).
		f := f + (self check: 'partial cancellation lands immediate'
			is: (((1.0 timesTwoPower: 258) + (1.0 timesTwoPower: 210)) - (1.0 timesTwoPower: 258))
				class == SmallFloat64).
		f := f + (self check: 'below-ulp addend is absorbed exactly'
			is: ((1.0 timesTwoPower: 258) + 1.0) = (1.0 timesTwoPower: 258)).
		^f
	]


	class multiplicationMatrix [
		| f |
		f := 0.
		f := f + (self check: 'imm * imm -> imm' is: (2.0 * 4.0) = 8.0).
		f := f + (self check: 'imm * boxed -> boxed' is: (2.0 * self giant) = (1.0 timesTwoPower: 401)).
		f := f + (self check: 'boxed * imm -> boxed' is: (self giant * 2.0) = (1.0 timesTwoPower: 401)).
		f := f + (self check: 'boxed * boxed -> boxed (2^800 is still finite)'
			is: (self giant * self giant) = (1.0 timesTwoPower: 800)).
		f := f + (self check: 'boxed * boxed -> overflow inf'
			is: ((1.0 timesTwoPower: 600) * (1.0 timesTwoPower: 600)) = Float infinity).
		f := f + (self check: 'imm * imm -> boxed (2^200 squared)'
			is: ((1.0 timesTwoPower: 200) * (1.0 timesTwoPower: 200)) = self giant).
		f := f + (self check: 'imm * imm -> boxed class'
			is: ((1.0 timesTwoPower: 200) * (1.0 timesTwoPower: 200)) class == BoxedFloat64).
		f := f + (self check: 'boxed * boxed -> IMM (giant times tiny)'
			is: (self giant * self tiny) = 1.0).
		f := f + (self check: 'giant times tiny class'
			is: (self giant * self tiny) class == SmallFloat64).
		f := f + (self check: 'boxed * imm -> imm (shrinks into range)'
			is: ((1.0 timesTwoPower: 257) * 0.25) = (1.0 timesTwoPower: 255)).
		f := f + (self check: 'shrink-into-range class'
			is: ((1.0 timesTwoPower: 257) * 0.25) class == SmallFloat64).
		f := f + (self check: 'tiny * tiny -> boxed (2^-800 is still normal)'
			is: (self tiny * self tiny) = (1.0 timesTwoPower: -800)).
		f := f + (self check: 'tiny * tiny underflows to imm zero'
			is: ((1.0 timesTwoPower: -600) * (1.0 timesTwoPower: -600)) = 0.0).
		f := f + (self check: 'underflowed product is immediate'
			is: ((1.0 timesTwoPower: -600) * (1.0 timesTwoPower: -600)) class == SmallFloat64).
		f := f + (self check: 'tiny * imm stays boxed tiny'
			is: (self tiny * 2.0) = (1.0 timesTwoPower: -399)).
		^f
	]


	class divisionMatrix [
		| f |
		f := 0.
		f := f + (self check: 'imm / imm -> imm' is: (1.0 / 4.0) = 0.25).
		f := f + (self check: 'boxed / boxed -> IMM (same magnitude)'
			is: (self giant / self giant) = 1.0).
		f := f + (self check: 'boxed / boxed -> imm class'
			is: (self giant / self giant) class == SmallFloat64).
		f := f + (self check: 'boxed / imm -> boxed' is: (self giant / 2.0) = (1.0 timesTwoPower: 399)).
		f := f + (self check: 'imm / boxed -> boxed tiny' is: (1.0 / self giant) = self tiny).
		f := f + (self check: 'imm / boxed tiny class' is: (1.0 / self giant) class == BoxedFloat64).
		f := f + (self check: 'imm / tiny-boxed -> giant' is: (1.0 / self tiny) = self giant).
		f := f + (self check: 'boxed / boxed -> boxed (2^-800 is still normal)'
			is: (self tiny / self giant) = (1.0 timesTwoPower: -800)).
		f := f + (self check: 'boxed / giant -> underflow imm zero'
			is: ((1.0 timesTwoPower: -600) / (1.0 timesTwoPower: 600)) = 0.0).
		f := f + (self check: 'imm / imm -> boxed (crosses down)'
			is: ((1.0 timesTwoPower: -200) / (1.0 timesTwoPower: 200)) = self tiny).
		f := f + (self check: 'giant / tiny -> boxed (2^800 is still finite)'
			is: (self giant / self tiny) = (1.0 timesTwoPower: 800)).
		f := f + (self check: 'giant / tiny -> overflow inf'
			is: ((1.0 timesTwoPower: 600) / (1.0 timesTwoPower: -600)) = Float infinity).
		^f
	]


	"---- walking the whole range in hot loops: the JIT intrinsic must box,
	unbox and re-encode correctly at EVERY step, in both directions"

	class boundaryWalk [
		| f x steps |
		f := 0.

		"up: 1.0 doubles 600 times to 2^600 (immediate until 2^256, boxed
		after), then halves 600 times straight back. Every value is an exact
		power of two, so the final compare is bit-exact."
		x := 1.0.
		steps := 0.
		[steps < 600] whileTrue: [x := x * 2.0. steps := steps + 1].
		f := f + (self check: 'walk up reaches 2^600' is: x = (1.0 timesTwoPower: 600)).
		f := f + (self check: '2^600 is boxed' is: x class == BoxedFloat64).
		steps := 0.
		[steps < 600] whileTrue: [x := x / 2.0. steps := steps + 1].
		f := f + (self check: 'walk down returns to 1.0 exactly' is: x = 1.0).
		f := f + (self check: 'walk result is immediate again' is: x class == SmallFloat64).

		"down through the boxed-tiny region and back up: 2^-1000 is a normal
		double far below the immediate floor"
		x := 1.0.
		steps := 0.
		[steps < 1000] whileTrue: [x := x / 2.0. steps := steps + 1].
		f := f + (self check: 'walk down reaches 2^-1000' is: x = (1.0 timesTwoPower: -1000)).
		f := f + (self check: '2^-1000 is boxed' is: x class == BoxedFloat64).
		steps := 0.
		[steps < 1000] whileTrue: [x := x * 2.0. steps := steps + 1].
		f := f + (self check: 'walk back up returns to 1.0 exactly' is: x = 1.0).
		f := f + (self check: 'walk up result is immediate again' is: x class == SmallFloat64).

		"a sum that flips representation exactly at the boundary, both ways"
		x := (1.0 timesTwoPower: 256).
		x := x + x.                       "2^257: boxes"
		x := x - (1.0 timesTwoPower: 256). "back to 2^256: immediate"
		f := f + (self check: 'flip at the boundary is exact' is: x = (1.0 timesTwoPower: 256)).
		f := f + (self check: 'flip lands immediate' is: x class == SmallFloat64).
		^f
	]


	"---- subnormal arithmetic: exact scaling through gradual underflow"

	class subnormalArithmetic [
		| f minNormal x |
		f := 0.
		minNormal := 2.2250738585072014e-308.
		f := f + (self check: 'subnormal + subnormal is exact'
			is: (5e-324 + 5e-324) = (5e-324 * 2.0)).
		f := f + (self check: 'subnormal sum grows' is: (5e-324 + 5e-324) > 5e-324).
		f := f + (self check: 'subnormal stays boxed' is: (5e-324 + 5e-324) class == BoxedFloat64).
		f := f + (self check: 'minNormal / 2 * 2 is exact (power of two)'
			is: (minNormal / 2.0 * 2.0) = minNormal).
		f := f + (self check: 'subnormal - itself is imm zero'
			is: (5e-324 - 5e-324) = 0.0).
		f := f + (self check: 'subnormal / giant is imm zero'
			is: (5e-324 / self giant) = 0.0).

		"walk from 1.0 down THROUGH the subnormal region (2^-1074 is the last
		one) and all the way back: every halving is exact for powers of two"
		x := 1.0.
		1 to: 1074 do: [:i | x := x / 2.0].
		f := f + (self check: 'reaches the last subnormal 2^-1074' is: x > 0.0).
		f := f + (self check: 'one more halving underflows to zero' is: (x / 2.0) = 0.0).
		1 to: 1074 do: [:i | x := x * 2.0].
		f := f + (self check: 'subnormal walk returns to 1.0 exactly' is: x = 1.0).
		^f
	]


	"---- infinities and NaN mixed with immediates through the intrinsic"

	class specialsMixed [
		| f inf |
		f := 0.
		inf := Float infinity.
		f := f + (self check: 'imm + inf = inf' is: (2.5 + inf) = inf).
		f := f + (self check: 'imm - inf = -inf' is: (2.5 - inf) = (0.0 - inf)).
		f := f + (self check: 'imm / inf is imm zero' is: (2.5 / inf) = 0.0).
		f := f + (self check: 'imm / inf class' is: (2.5 / inf) class == SmallFloat64).
		f := f + (self check: 'inf / imm = inf' is: (inf / 2.5) = inf).
		f := f + (self check: 'imm * NaN is NaN' is: (2.5 * Float nan) isNaN).
		f := f + (self check: 'NaN * imm is NaN' is: (Float nan * 2.5) isNaN).
		f := f + (self check: 'NaN result boxes' is: (2.5 * Float nan) class == BoxedFloat64).
		f := f + (self check: 'imm < boxed giant' is: 2.5 < self giant).
		f := f + (self check: 'boxed giant > imm' is: self giant > 2.5).
		f := f + (self check: 'boxed tiny < imm' is: self tiny < 2.5).
		f := f + (self check: 'imm ~= boxed giant' is: 2.5 ~= self giant).
		f := f + (self check: 'giant = giant across boxes'
			is: self giant = self giant).
		^f
	]


	"---- unary primitives crossing representations"

	class unaryCrossing [
		| f |
		f := 0.
		f := f + (self check: 'sqrt of imm stays imm' is: 4.0 sqrt = 2.0).
		f := f + (self check: 'sqrt SHRINKS a boxed giant into the imm range'
			is: self giant sqrt = (1.0 timesTwoPower: 200)).
		f := f + (self check: 'sqrt of giant lands immediate'
			is: self giant sqrt class == SmallFloat64).
		f := f + (self check: 'sqrt of 2^600 stays boxed'
			is: (1.0 timesTwoPower: 600) sqrt class == BoxedFloat64).
		f := f + (self check: 'sqrt of 2^400 squared round-trips'
			is: (self giant sqrt * self giant sqrt) = self giant).
		f := f + (self check: 'sqrt LIFTS a boxed tiny into the imm range'
			is: self tiny sqrt = (1.0 timesTwoPower: -200)).
		f := f + (self check: 'sqrt of tiny lands immediate'
			is: self tiny sqrt class == SmallFloat64).
		f := f + (self check: 'huge integer asFloat overflows to inf'
			is: (2 raisedToInteger: 1200) asFloat = Float infinity).
		f := f + (self check: 'giant truncated round-trips exactly'
			is: self giant truncated asFloat = self giant).
		^f
	]


	class runAll [
		| total |
		total := self additionMatrix.
		total := total + self subtractionMatrix.
		total := total + self multiplicationMatrix.
		total := total + self divisionMatrix.
		total := total + self boundaryWalk.
		total := total + self subnormalArithmetic.
		total := total + self specialsMixed.
		total := total + self unaryCrossing.
		total = 0
			ifTrue: [Transcript nextPutAll: 'FloatCrossRepTest: all sections passed'; lf]
			ifFalse: [Transcript nextPutAll: 'FloatCrossRepTest: ', total printString, ' FAILURES'; lf].
		^total
	]

]


[
	^FloatCrossRepTest runAll
]
