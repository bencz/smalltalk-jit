"MIXED-TYPE arithmetic across the whole numeric tower.

 Written BEFORE the coercion path was touched and asserting the behaviour that
 already existed, so it is a pin, not a specification of new behaviour: no
 expected value in this file may change when Number>>retry:coercing: stops
 going through perform:, nor when the JIT learns to take an int/float operand
 pair in its float fast path.

 The tower is SmallInteger < LargeInteger < Fraction < ScaledDecimal < Float by
 generality, and a mixed operation coerces the lower operand up. The matrix
 below drives every ordered pair of representations through every arithmetic
 and relational operator and checks both the VALUE and, where it is the point,
 the resulting CLASS.

 Two entries here are traps for a future int/float fast path in the JIT, and
 they are the reason this file must be green before any such path is written:

   * `3 / 2` MUST answer the Fraction 3/2. Integer division is deliberately
     excluded from the integer fast path, so with a careless guard the pair
     reaches the FLOAT path and silently answers 1.5.
   * `SmallInteger maxVal + 1` MUST promote to LargePositiveInteger. The
     integer path's overflow exit falls THROUGH into the float path, so the
     same careless guard silently answers a Float.

 Both would be wrong answers with no error, in the most-used operator in the
 language."

[ | t big frac sd f i mismatches all |
	t := TestRun named: 'MixedArith'.

	i := 3.
	big := 2 raisedTo: 100.
	frac := 3/4.
	sd := 1.5s2.
	f := 2.5.

	"--- the two traps ---"
	t check: '3 / 2 is a Fraction, not 1.5' equals: Fraction is: [ (3 / 2) class ].
	t check: '3 / 2 is exactly 3/2' equals: (3/2) is: [ 3 / 2 ].
	t check: '4 / 2 demotes to an Integer' equals: 2 is: [ 4 / 2 ].
	t check: 'maxVal + 1 promotes, it does not become a Float'
		equals: LargePositiveInteger is: [ (SmallInteger maxVal + 1) class ].
	t check: 'minVal - 1 promotes too'
		equals: LargeNegativeInteger is: [ (SmallInteger minVal - 1) class ].
	t check: 'maxVal * 2 promotes' equals: LargePositiveInteger
		is: [ (SmallInteger maxVal * 2) class ].

	"--- SmallInteger with Float, both operand orders ---"
	t check: 'int + float' equals: 5.5 is: [ 3 + 2.5 ].
	t check: 'float + int' equals: 5.5 is: [ 2.5 + 3 ].
	t check: 'int - float' equals: 0.5 is: [ 3 - 2.5 ].
	t check: 'float - int' equals: -0.5 is: [ 2.5 - 3 ].
	t check: 'int * float' equals: 7.5 is: [ 3 * 2.5 ].
	t check: 'float * int' equals: 7.5 is: [ 2.5 * 3 ].
	t check: 'int / float' equals: 1.2 is: [ 3 / 2.5 ].
	t check: 'float / int' equals: 1.25 is: [ 2.5 / 2 ].
	t check: 'int + float answers a Float' equals: SmallFloat64 is: [ (3 + 2.5) class ].
	t check: 'float + int answers a Float' equals: SmallFloat64 is: [ (2.5 + 3) class ].

	"relational, both orders, including the equal case"
	t check: 'int < float' equals: false is: [ 3 < 2.5 ].
	t check: 'float < int' equals: true is: [ 2.5 < 3 ].
	t check: 'int > float' equals: true is: [ 3 > 2.5 ].
	t check: 'float > int' equals: false is: [ 2.5 > 3 ].
	t check: 'int <= float' equals: false is: [ 3 <= 2.5 ].
	t check: 'float <= int' equals: true is: [ 2.5 <= 3 ].
	t check: 'int >= float' equals: true is: [ 3 >= 2.5 ].
	t check: 'float >= int' equals: false is: [ 2.5 >= 3 ].
	t check: 'int = equal float' equals: true is: [ 3 = 3.0 ].
	t check: 'float = equal int' equals: true is: [ 3.0 = 3 ].
	t check: 'int ~= equal float' equals: false is: [ 3 ~= 3.0 ].
	t check: 'int = unequal float' equals: false is: [ 3 = 2.5 ].
	t check: 'int <= equal float' equals: true is: [ 3 <= 3.0 ].
	t check: 'int >= equal float' equals: true is: [ 3 >= 3.0 ].

	"floored division and modulo across the boundary, including negatives"
	t check: 'int // float' equals: 1 is: [ 3 // 2.5 ].
	t check: 'float // int' equals: 0 is: [ 2.5 // 3 ].
	t check: 'negative int // float floors' equals: -2 is: [ -3 // 2.5 ].
	t check: 'int \\ float' equals: 0.5 is: [ 3 \\ 2.5 ].
	t check: 'float \\ int' equals: 2.5 is: [ 2.5 \\ 3 ].

	"--- SmallInteger with Fraction ---"
	t check: 'int + fraction' equals: (15/4) is: [ 3 + frac ].
	t check: 'fraction + int' equals: (15/4) is: [ frac + 3 ].
	t check: 'int - fraction' equals: (9/4) is: [ 3 - frac ].
	t check: 'fraction - int' equals: (-9/4) is: [ frac - 3 ].
	t check: 'int * fraction' equals: (9/4) is: [ 3 * frac ].
	t check: 'fraction * int' equals: (9/4) is: [ frac * 3 ].
	t check: 'int / fraction' equals: 4 is: [ 3 / frac ].
	t check: 'fraction / int' equals: (1/4) is: [ frac / 3 ].
	t check: 'int > fraction' equals: true is: [ 3 > frac ].
	t check: 'fraction < int' equals: true is: [ frac < 3 ].
	t check: 'fraction = equal int' equals: true is: [ (4/2) = 2 ].

	"--- Fraction with Float ---"
	t check: 'fraction + float' equals: 3.25 is: [ frac + f ].
	t check: 'float + fraction' equals: 3.25 is: [ f + frac ].
	t check: 'fraction * float' equals: 1.875 is: [ frac * f ].
	t check: 'fraction < float' equals: true is: [ frac < f ].
	t check: 'float > fraction' equals: true is: [ f > frac ].
	t check: 'fraction + float answers a Float' equals: SmallFloat64 is: [ (frac + f) class ].

	"--- LargeInteger with the rest ---"
	t check: 'large + int' equals: (2 raisedTo: 100) + 1 is: [ big + 1 ].
	t check: 'int + large' equals: (2 raisedTo: 100) + 1 is: [ 1 + big ].
	t check: 'large - large is exact' equals: 0 is: [ big - big ].
	t check: 'large * int' equals: (2 raisedTo: 101) is: [ big * 2 ].
	t check: 'large > int' equals: true is: [ big > 1 ].
	t check: 'int < large' equals: true is: [ 1 < big ].
	t check: 'large > float' equals: true is: [ big > 1.0 ].
	t check: 'float < large' equals: true is: [ 1.0 < big ].
	t check: 'large + float is a Float' equals: true is: [ (big + 1.0) isFloat ].
	t check: 'large demotes when it fits again' equals: SmallInteger
		is: [ (big - big + 5) class ].

	"--- ScaledDecimal, which sits between Fraction and Float ---"
	t check: 'scaled + int' equals: 4.5s2 is: [ sd + 3 ].
	t check: 'int + scaled' equals: 4.5s2 is: [ 3 + sd ].
	t check: 'scaled * int' equals: 4.5s2 is: [ sd * 3 ].
	t check: 'scaled < int' equals: true is: [ sd < 3 ].
	t check: 'int > scaled' equals: true is: [ 3 > sd ].
	t check: 'scaled + float is a Float' equals: true is: [ (sd + f) isFloat ].
	t check: 'scaled = equal int' equals: true is: [ 2s0 = 2 ].

	"--- commutativity of + and *, and antisymmetry of <, over the matrix ---"
	mismatches := 0.
	all := Array new: 5.
	all at: 1 put: 3; at: 2 put: big; at: 3 put: frac; at: 4 put: sd; at: 5 put: f.
	all do: [:a |
		all do: [:b |
			(a + b) = (b + a) ifFalse: [mismatches := mismatches + 1].
			(a * b) = (b * a) ifFalse: [mismatches := mismatches + 1].
			((a < b) and: [b < a]) ifTrue: [mismatches := mismatches + 1].
			((a = b) and: [(a < b) or: [b < a]]) ifTrue: [mismatches := mismatches + 1]]].
	t check: '+ and * commute and < is antisymmetric over the 5x5 matrix'
		equals: 0 is: [ mismatches ].

	"--- coercion must not lose the answer for a non-Number ---"
	t expect: 'int + a String raises' raises: Error in: [ 3 + 'x' ].
	t expect: 'float + a String raises' raises: Error in: [ 2.5 + 'x' ].
	t expect: 'int + nil raises' raises: Error in: [ 3 + nil ].

	^t report
]
