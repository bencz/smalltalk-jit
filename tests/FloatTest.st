"Split into small top-level blocks to stay under the compiler's large-method
limit. Assertions use exactly representable values or integer results to avoid
floating point rounding surprises."

[
	"literals and class"
	Assert true: 3.14 class == SmallFloat64.
	Assert true: 1.0 class == SmallFloat64.
	Assert true: 1.0e300 class == BoxedFloat64.
	Assert true: (3.14 isKindOf: Float).
	Assert true: (1.0e300 isKindOf: Float).
	Assert true: Float infinity class == BoxedFloat64.
	Assert true: Float infinity = Float infinity.
	Assert true: (2.0 + 3.5) class == SmallFloat64.
	Assert true: 3 class == SmallInteger.
	Assert true: 0.5 = 0.5.
	Assert true: 1.5e3 = 1500.0.
	Assert true: 2.5e-1 = 0.25.
	Assert true: 3.14 printString = '3.14'.
	Assert true: 1500.0 printString = '1500.0'.
]


[
	"arithmetic (exact binary fractions)"
	Assert true: 2.0 + 3.5 = 5.5.
	Assert true: 5.5 - 2.0 = 3.5.
	Assert true: 1.5 * 2.0 = 3.0.
	Assert true: 10.0 / 4.0 = 2.5.
	Assert true: 0.5 negated = -0.5.
	Assert true: (-0.5) abs = 0.5.
	Assert true: 2.0 sqrt * 2.0 sqrt - 2.0 < 0.0000001.
]


[
	"mixed integer / float coercion"
	Assert true: 1 + 2.5 = 3.5.
	Assert true: 2.5 + 1 = 3.5.
	Assert true: 2 * 1.5 = 3.0.
	Assert true: 1.5 * 2 = 3.0.
	Assert true: 3 - 0.5 = 2.5.
	Assert true: 7 / 2 = (7/2).
	Assert true: (1/2) asFloat = 0.5.
	Assert true: (1/4) + 0.25 = 0.5.
	Assert true: 2 sqrt = 2.0 sqrt.
]


[
	"comparing across types"
	Assert true: 2.5 < 3.
	Assert true: 3 > 2.5.
	Assert true: (2.5 < (5/2)) not.
	Assert true: 0.5 = (1/2).
	Assert true: 1.0 = 1.
	Assert true: (2.5 = 3) not.
]


[
	"truncation and rounding"
	Assert true: 3.7 truncated = 3.
	Assert true: 3.7 floor = 3.
	Assert true: 3.2 ceiling = 4.
	Assert true: 3.5 rounded = 4.
	Assert true: (-3.7) floor = -4.
	Assert true: (-3.7) ceiling = -3.
	Assert true: (-3.7) truncated = -3.
	Assert true: 3.7 truncated class == SmallInteger.
	Assert true: 3.0 asInteger = 3.
]


[
	"constants and functions"
	Assert true: (Float pi - 3.141592653589793) abs < 0.0000001.
	Assert true: 4.0 sqrt = 2.0.
	Assert true: (0.0 sin) = 0.0.
	Assert true: (0.0 cos) = 1.0.
	Assert true: 100 factorial asFloat > 1.0e100.

	"errors for non-numbers"
	Assert do: [1.5 + Object] expect: Error.
	Assert do: [1.5 < Object] expect: Error.
]
