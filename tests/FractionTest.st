"Split into several small top-level blocks: the compiler aborts on very large
single methods, so each block stays well under that limit."

[
	"construction, reduction and demotion to Integer"
	Assert true: (1/2) class == Fraction.
	Assert true: (10/2) class == SmallInteger.
	Assert true: (10/2) = 5.
	Assert true: (4/6) = (2/3).
	Assert true: (2/3) numerator = 2.
	Assert true: (2/3) denominator = 3.
	Assert true: (6/(-4)) = (-3/2).
	Assert true: (6/(-4)) denominator = 2.
]


[
	"arithmetic within fractions"
	Assert true: (1/2) + (1/3) = (5/6).
	Assert true: (1/2) - (1/2) = 0.
	Assert true: ((1/2) - (1/2)) class == SmallInteger.
	Assert true: (2/3) * (3/4) = (1/2).
	Assert true: (1/2) / (1/4) = 2.
	Assert true: (2/3) reciprocal = (3/2).
	Assert true: (1/2) squared = (1/4).
]


[
	"mixed integer / fraction arithmetic (coercion)"
	Assert true: (1/2) + 1 = (3/2).
	Assert true: 1 + (1/2) = (3/2).
	Assert true: 2 * (1/2) = 1.
	Assert true: (1/2) * 2 = 1.
	Assert true: 3 - (1/2) = (5/2).
	Assert true: (1/2) - 3 = (-5/2).
	Assert true: 1 / 3 = (1/3).
]


[
	"large fractions and comparing"
	Assert true: (1 / (2 raisedTo: 70)) denominator = (2 raisedTo: 70).
	Assert true: ((2 raisedTo: 70) / (2 raisedTo: 71)) = (1/2).
	Assert true: (3/4) < (5/6).
	Assert true: (1/2) < 1.
	Assert true: 3 > (1/2).
	Assert true: ((1/2) = (2/4)).
	Assert true: ((1/2) = 1) not.
	Assert true: (1 = (1/2)) not.
]


[
	"truncation, rounding and negative exponent"
	Assert true: (7/2) truncated = 3.
	Assert true: (7/2) floor = 3.
	Assert true: (-7/2) floor = -4.
	Assert true: (-7/2) truncated = -3.
	Assert true: (7/2) ceiling = 4.
	Assert true: (3/4) rounded = 1.
	Assert true: (1/4) rounded = 0.
	Assert true: (2 raisedTo: -3) = (1/8).
]


[
	"errors for non-numbers"
	Assert do: [(1/2) + Object] expect: Error.
	Assert do: [(1/2) < Object] expect: Error.
]
