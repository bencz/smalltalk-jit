"Split into several small top-level blocks to stay under the compiler's
large-method limit."

[
	"promotion on overflow and large literals: the sign is in the CLASS
	(Pharo style), LargeInteger itself is abstract"
	LargeIntegerRun := TestRun named: 'LargeInteger'.

	LargeIntegerRun check: '2^62 class' that: [ (2 raisedTo: 62) class == LargePositiveInteger ].
	LargeIntegerRun check: '2^62 isKindOf LargeInteger' that: [ (2 raisedTo: 62) isKindOf: LargeInteger ].
	LargeIntegerRun check: '1e9 class small' that: [ 1000000000 class == SmallInteger ].
	LargeIntegerRun check: '1e9 squared class small' that: [ (1000000000 * 1000000000) class == SmallInteger ].
	LargeIntegerRun check: '1e10 squared class large' that: [ (10000000000 * 10000000000) class == LargePositiveInteger ].
	LargeIntegerRun check: '1e10 squared value' equals: 100000000000000000000 is: [ 10000000000 * 10000000000 ].
	LargeIntegerRun check: '20 factorial class' that: [ 2432902008176640000 class == LargePositiveInteger ].
	LargeIntegerRun check: '20 factorial value' equals: 2432902008176640000 is: [ 20 factorial ].
	LargeIntegerRun check: 'large printString size' equals: 33 is: [ 100000000000000000000000000000000 printString size ].
	LargeIntegerRun check: 'radix 16 literal' equals: 1208925819614629174706175 is: [ 16rFFFFFFFFFFFFFFFFFFFF ].
]


[
	"arithmetic"
	LargeIntegerRun check: '2^100 plus 1' equals: 1267650600228229401496703205377 is: [ (2 raisedTo: 100) + 1 ].
	LargeIntegerRun check: '2^100 self-sub' equals: 0 is: [ (2 raisedTo: 100) - (2 raisedTo: 100) ].
	LargeIntegerRun check: 'self-sub demotes' that: [ ((2 raisedTo: 100) - (2 raisedTo: 100)) class == SmallInteger ].
	LargeIntegerRun check: '2^64 idiv 2^32' that: [ (2 raisedTo: 64) // (2 raisedTo: 32) = (2 raisedTo: 32) ].
	LargeIntegerRun check: '2^64 mod 7' equals: 2 is: [ (2 raisedTo: 64) \\ 7 ].
	LargeIntegerRun check: '100 factorial digits' equals: 158 is: [ (100 factorial) printString size ].
	LargeIntegerRun check: '2^70 self-div' equals: 1 is: [ (2 raisedTo: 70) // (2 raisedTo: 70) ].
	LargeIntegerRun check: 'self-div demotes' that: [ ((2 raisedTo: 70) // (2 raisedTo: 70)) class == SmallInteger ].
]


[
	"sign handling and floored division with mixed signs"
	LargeIntegerRun check: 'negated lt zero' that: [ (2 raisedTo: 70) negated < 0 ].
	LargeIntegerRun check: 'negated class' that: [ (2 raisedTo: 70) negated class == LargeNegativeInteger ].
	LargeIntegerRun check: 'negated isKindOf LargeInteger' that: [ (2 raisedTo: 70) negated isKindOf: LargeInteger ].
	LargeIntegerRun check: 'negated abs class' that: [ (2 raisedTo: 70) negated abs class == LargePositiveInteger ].
	LargeIntegerRun check: 'neg times neg class' that: [ ((2 raisedTo: 70) negated * (2 raisedTo: 70) negated) class == LargePositiveInteger ].
	LargeIntegerRun check: 'pos times neg class' that: [ ((2 raisedTo: 70) * (2 raisedTo: 70) negated) class == LargeNegativeInteger ].
	LargeIntegerRun check: 'zero minus large class' that: [ (0 - 100000000000000000000) class == LargeNegativeInteger ].
	LargeIntegerRun check: 'negated abs value' that: [ (2 raisedTo: 70) negated abs = (2 raisedTo: 70) ].
	LargeIntegerRun check: 'negated sign' equals: -1 is: [ ((2 raisedTo: 70) negated) sign ].
	LargeIntegerRun check: 'neg plus pos cancels' equals: 0 is: [ (0 - (2 raisedTo: 65)) + (2 raisedTo: 65) ].
	LargeIntegerRun check: 'floored div mixed signs' equals: -773 is: [ 273968123 // -354520 ].
	LargeIntegerRun check: '7 idiv -2' equals: -4 is: [ 7 // -2 ].
	LargeIntegerRun check: '7 mod -2' equals: -1 is: [ 7 \\ -2 ].
	LargeIntegerRun check: 'div neg matches neg div' that: [ ((2 raisedTo: 80) // -3) = ((2 raisedTo: 80) negated // 3) ].
]


[
	"comparing, gcd, parity and errors"
	LargeIntegerRun check: '2^70 eq 2^69 doubled' that: [ (2 raisedTo: 70) = ((2 raisedTo: 69) * 2) ].
	LargeIntegerRun check: '2^70 gt 2^69' that: [ (2 raisedTo: 70) > (2 raisedTo: 69) ].
	LargeIntegerRun check: 'large ne small' that: [ ((2 raisedTo: 100) = 5) not ].
	LargeIntegerRun check: 'gcd powers of two' that: [ ((2 raisedTo: 100) gcd: (2 raisedTo: 60)) = (2 raisedTo: 60) ].
	LargeIntegerRun check: '2^100 even' that: [ (2 raisedTo: 100) even ].
	LargeIntegerRun check: '2^100 plus 1 odd' that: [ ((2 raisedTo: 100) + 1) odd ].
	LargeIntegerRun expect: 'large add non-number' raises: Error in: [(2 raisedTo: 70) + Object].
	LargeIntegerRun expect: 'large lt non-number' raises: Error in: [(2 raisedTo: 70) < Object].
]

[ ^LargeIntegerRun report ]
