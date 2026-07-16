"Split into several small top-level blocks to stay under the compiler's
large-method limit."

[
	"promotion on overflow and large literals: the sign is in the CLASS
	(Pharo style), LargeInteger itself is abstract"
	Assert true: (2 raisedTo: 62) class == LargePositiveInteger.
	Assert true: ((2 raisedTo: 62) isKindOf: LargeInteger).
	Assert true: 1000000000 class == SmallInteger.
	Assert true: (1000000000 * 1000000000) class == SmallInteger.
	Assert true: (10000000000 * 10000000000) class == LargePositiveInteger.
	Assert true: (10000000000 * 10000000000) = 100000000000000000000.
	Assert true: 2432902008176640000 class == LargePositiveInteger.
	Assert true: 20 factorial = 2432902008176640000.
	Assert true: 100000000000000000000000000000000 printString size = 33.
	Assert true: 16rFFFFFFFFFFFFFFFFFFFF = 1208925819614629174706175.
]


[
	"arithmetic"
	Assert true: (2 raisedTo: 100) + 1 = 1267650600228229401496703205377.
	Assert true: (2 raisedTo: 100) - (2 raisedTo: 100) = 0.
	Assert true: ((2 raisedTo: 100) - (2 raisedTo: 100)) class == SmallInteger.
	Assert true: (2 raisedTo: 64) // (2 raisedTo: 32) = (2 raisedTo: 32).
	Assert true: ((2 raisedTo: 64) \\ 7) = 2.
	Assert true: (100 factorial) printString size = 158.
	Assert true: ((2 raisedTo: 70) // (2 raisedTo: 70)) = 1.
	Assert true: ((2 raisedTo: 70) // (2 raisedTo: 70)) class == SmallInteger.
]


[
	"sign handling and floored division with mixed signs"
	Assert true: (2 raisedTo: 70) negated < 0.
	Assert true: (2 raisedTo: 70) negated class == LargeNegativeInteger.
	Assert true: ((2 raisedTo: 70) negated isKindOf: LargeInteger).
	Assert true: (2 raisedTo: 70) negated abs class == LargePositiveInteger.
	Assert true: ((2 raisedTo: 70) negated * (2 raisedTo: 70) negated) class == LargePositiveInteger.
	Assert true: ((2 raisedTo: 70) * (2 raisedTo: 70) negated) class == LargeNegativeInteger.
	Assert true: (0 - 100000000000000000000) class == LargeNegativeInteger.
	Assert true: (2 raisedTo: 70) negated abs = (2 raisedTo: 70).
	Assert true: ((2 raisedTo: 70) negated) sign = -1.
	Assert true: (0 - (2 raisedTo: 65)) + (2 raisedTo: 65) = 0.
	Assert true: (273968123 // -354520) = -773.
	Assert true: (7 // -2) = -4.
	Assert true: (7 \\ -2) = -1.
	Assert true: ((2 raisedTo: 80) // -3) = ((2 raisedTo: 80) negated // 3).
]


[
	"comparing, gcd, parity and errors"
	Assert true: (2 raisedTo: 70) = ((2 raisedTo: 69) * 2).
	Assert true: (2 raisedTo: 70) > (2 raisedTo: 69).
	Assert true: ((2 raisedTo: 70) = 5) not.
	Assert true: ((2 raisedTo: 100) gcd: (2 raisedTo: 60)) = (2 raisedTo: 60).
	Assert true: (2 raisedTo: 100) even.
	Assert true: ((2 raisedTo: 100) + 1) odd.
	Assert do: [(2 raisedTo: 70) + Object] expect: Error.
	Assert do: [(2 raisedTo: 70) < Object] expect: Error.
]
