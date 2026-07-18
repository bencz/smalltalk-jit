"Split into small top-level blocks to stay under the compiler's large-method
limit. Assertions use exactly representable values or integer results to avoid
floating point rounding surprises."

[
	"literals and class"
	FloatRun := TestRun named: 'Float'.

	FloatRun check: '3.14 class' that: [ 3.14 class == SmallFloat64 ].
	FloatRun check: '1.0 class' that: [ 1.0 class == SmallFloat64 ].
	FloatRun check: '1.0e300 class' that: [ 1.0e300 class == BoxedFloat64 ].
	FloatRun check: '3.14 isKindOf Float' that: [ 3.14 isKindOf: Float ].
	FloatRun check: '1.0e300 isKindOf Float' that: [ 1.0e300 isKindOf: Float ].
	FloatRun check: 'infinity class' that: [ Float infinity class == BoxedFloat64 ].
	FloatRun check: 'infinity eq itself' that: [ Float infinity = Float infinity ].
	FloatRun check: 'sum class small' that: [ (2.0 + 3.5) class == SmallFloat64 ].
	FloatRun check: '3 class' that: [ 3 class == SmallInteger ].
	FloatRun check: '0.5 eq 0.5' that: [ 0.5 = 0.5 ].
	FloatRun check: 'exponent literal' equals: 1500.0 is: [ 1.5e3 ].
	FloatRun check: 'negative exponent literal' equals: 0.25 is: [ 2.5e-1 ].
	FloatRun check: '3.14 printString' equals: '3.14' is: [ 3.14 printString ].
	FloatRun check: '1500.0 printString' equals: '1500.0' is: [ 1500.0 printString ].
]


[
	"arithmetic (exact binary fractions)"
	FloatRun check: 'add' equals: 5.5 is: [ 2.0 + 3.5 ].
	FloatRun check: 'sub' equals: 3.5 is: [ 5.5 - 2.0 ].
	FloatRun check: 'mul' equals: 3.0 is: [ 1.5 * 2.0 ].
	FloatRun check: 'div' equals: 2.5 is: [ 10.0 / 4.0 ].
	FloatRun check: 'negated' equals: -0.5 is: [ 0.5 negated ].
	FloatRun check: 'abs' equals: 0.5 is: [ (-0.5) abs ].
	FloatRun check: 'sqrt squared near 2' that: [ 2.0 sqrt * 2.0 sqrt - 2.0 < 0.0000001 ].
]


[
	"mixed integer / float coercion"
	FloatRun check: 'int plus float' equals: 3.5 is: [ 1 + 2.5 ].
	FloatRun check: 'float plus int' equals: 3.5 is: [ 2.5 + 1 ].
	FloatRun check: 'int times float' equals: 3.0 is: [ 2 * 1.5 ].
	FloatRun check: 'float times int' equals: 3.0 is: [ 1.5 * 2 ].
	FloatRun check: 'int minus float' equals: 2.5 is: [ 3 - 0.5 ].
	FloatRun check: 'int div gives fraction' equals: (7/2) is: [ 7 / 2 ].
	FloatRun check: 'fraction asFloat' equals: 0.5 is: [ (1/2) asFloat ].
	FloatRun check: 'fraction plus float' equals: 0.5 is: [ (1/4) + 0.25 ].
	FloatRun check: 'int sqrt eq float sqrt' that: [ 2 sqrt = 2.0 sqrt ].
]


[
	"comparing across types"
	FloatRun check: 'float lt int' that: [ 2.5 < 3 ].
	FloatRun check: 'int gt float' that: [ 3 > 2.5 ].
	FloatRun check: 'float not lt equal fraction' that: [ (2.5 < (5/2)) not ].
	FloatRun check: 'float eq fraction' that: [ 0.5 = (1/2) ].
	FloatRun check: 'float eq int' that: [ 1.0 = 1 ].
	FloatRun check: 'float ne int' that: [ (2.5 = 3) not ].
]


[
	"truncation and rounding"
	FloatRun check: 'truncated' equals: 3 is: [ 3.7 truncated ].
	FloatRun check: 'floor' equals: 3 is: [ 3.7 floor ].
	FloatRun check: 'ceiling' equals: 4 is: [ 3.2 ceiling ].
	FloatRun check: 'rounded' equals: 4 is: [ 3.5 rounded ].
	FloatRun check: 'neg floor' equals: -4 is: [ (-3.7) floor ].
	FloatRun check: 'neg ceiling' equals: -3 is: [ (-3.7) ceiling ].
	FloatRun check: 'neg truncated' equals: -3 is: [ (-3.7) truncated ].
	FloatRun check: 'truncated class' that: [ 3.7 truncated class == SmallInteger ].
	FloatRun check: 'asInteger' equals: 3 is: [ 3.0 asInteger ].
]


[
	"constants and functions"
	FloatRun check: 'pi approx' that: [ (Float pi - 3.141592653589793) abs < 0.0000001 ].
	FloatRun check: 'sqrt exact' equals: 2.0 is: [ 4.0 sqrt ].
	FloatRun check: 'sin zero' equals: 0.0 is: [ 0.0 sin ].
	FloatRun check: 'cos zero' equals: 1.0 is: [ 0.0 cos ].
	FloatRun check: '100 factorial asFloat big' that: [ 100 factorial asFloat > 1.0e100 ].

	"errors for non-numbers"
	FloatRun expect: 'float add non-number' raises: Error in: [1.5 + Object].
	FloatRun expect: 'float lt non-number' raises: Error in: [1.5 < Object].
]

[ ^FloatRun report ]
