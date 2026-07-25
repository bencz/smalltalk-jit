"bitShift: across sign, magnitude and shift count.

 Three defects lived in generateIntShiftPrimitive, all producing silent wrong
 answers, and all covered here:

  1. RIGHT SHIFT WAS LOGICAL. `shr` on x64 and `srd` on ppc64 do not preserve
     the sign, so `-8 bitShift: -1` answered 2305843009213693948 instead of -4.
     Smalltalk's bitShift: is arithmetic, so a right shift must agree with
     floored division by a power of two for EVERY receiver, which is the
     property the sweep below asserts directly.

  2. LEFT SHIFT NEVER CHECKED OVERFLOW, so `1 bitShift: 100` silently truncated
     to 2^36 instead of promoting to LargeInteger.

  3. THE COUNT WAS NEITHER CLAMPED NOR RANGE-CHECKED, and the backends
     DISAGREED because of it: x86 masks a shift count to 6 bits (so
     `1 bitShift: 64` answered 1) while POWER uses 7 bits (answering 0). Only
     the low BYTE of the count was read, too, so a shift by -256 degenerated
     into a shift by 0. This file therefore gates on ppc64 as well as x64.

 Previous coverage was four positive-receiver cases in IntegerProtocolTest."

[ | t mismatches shiftBy |
	t := TestRun named: 'BitShift'.

	"--- 1. arithmetic right shift: the defining property, swept ---"
	mismatches := 0.
	#(-1000000 -1025 -1024 -1023 -9 -8 -7 -3 -2 -1 0 1 2 3 7 8 9 1023 1024 1025 1000000)
		do: [:x |
			1 to: 20 do: [:n |
				(x bitShift: n negated) = (x // (2 raisedTo: n))
					ifFalse: [mismatches := mismatches + 1]]].
	t check: 'right shift equals floored division by 2^n, over 21 x 20 cases'
		equals: 0 is: [ mismatches ].

	"the specific case that was wrong, spelled out"
	t check: '-8 bitShift: -1' equals: -4 is: [ -8 bitShift: -1 ].
	t check: '-1 bitShift: -1 stays -1 (floor, not toward zero)' equals: -1 is: [ -1 bitShift: -1 ].
	t check: '-3 bitShift: -1 floors to -2' equals: -2 is: [ -3 bitShift: -1 ].
	t check: '-7 bitShift: -2 floors to -2' equals: -2 is: [ -7 bitShift: -2 ].

	"--- 2. left shift overflow promotes instead of truncating ---"
	t check: '1 bitShift: 100 is exactly 2^100' equals: (2 raisedTo: 100)
		is: [ 1 bitShift: 100 ].
	t check: 'and it is a LargeInteger' equals: LargePositiveInteger
		is: [ (1 bitShift: 100) class ].
	t check: '-1 bitShift: 100 is exactly -2^100' equals: (2 raisedTo: 100) negated
		is: [ -1 bitShift: 100 ].
	t check: 'left then right round-trips' equals: 1
		is: [ (1 bitShift: 100) bitShift: -100 ].
	t check: 'a big shift of a big receiver is exact' equals: (3 * (2 raisedTo: 200))
		is: [ 3 bitShift: 200 ].

	"the immediate/boxed boundary: 2^61 does not fit a 62-bit signed payload"
	t check: '1 bitShift: 60 stays immediate' equals: SmallInteger
		is: [ (1 bitShift: 60) class ].
	t check: '1 bitShift: 61 promotes' equals: LargePositiveInteger
		is: [ (1 bitShift: 61) class ].
	t check: '1 bitShift: 61 is exact' equals: (2 raisedTo: 61) is: [ 1 bitShift: 61 ].
	t check: 'shifting back demotes again' equals: SmallInteger
		is: [ ((1 bitShift: 61) bitShift: -1) class ].

	"--- 3. shift counts at and past the word size, where the CPUs differed ---"
	t check: '1 bitShift: 62' equals: (2 raisedTo: 62) is: [ 1 bitShift: 62 ].
	t check: '1 bitShift: 63' equals: (2 raisedTo: 63) is: [ 1 bitShift: 63 ].
	t check: '1 bitShift: 64 is 2^64, not 1 (x86) and not 0 (POWER)'
		equals: (2 raisedTo: 64) is: [ 1 bitShift: 64 ].
	t check: '1 bitShift: 65' equals: (2 raisedTo: 65) is: [ 1 bitShift: 65 ].

	"a right shift past the word size saturates to the mathematically exact
	 answer: 0 for a non-negative receiver, -1 for a negative one"
	t check: '8 bitShift: -63' equals: 0 is: [ 8 bitShift: -63 ].
	t check: '8 bitShift: -64' equals: 0 is: [ 8 bitShift: -64 ].
	t check: '8 bitShift: -65' equals: 0 is: [ 8 bitShift: -65 ].
	t check: '8 bitShift: -1000' equals: 0 is: [ 8 bitShift: -1000 ].
	t check: '-8 bitShift: -63 floors to -1' equals: -1 is: [ -8 bitShift: -63 ].
	t check: '-8 bitShift: -64 floors to -1' equals: -1 is: [ -8 bitShift: -64 ].
	t check: '-8 bitShift: -65 floors to -1' equals: -1 is: [ -8 bitShift: -65 ].
	t check: '-8 bitShift: -1000 floors to -1' equals: -1 is: [ -8 bitShift: -1000 ].

	"the count was read as a single BYTE, so multiples of 256 degenerated to 0"
	t check: '-8 bitShift: -256 is not -8' equals: -1 is: [ -8 bitShift: -256 ].
	t check: '8 bitShift: -256 is not 8' equals: 0 is: [ 8 bitShift: -256 ].
	shiftBy := -512.
	t check: '8 bitShift: -512 is not 8' equals: 0 is: [ 8 bitShift: shiftBy ].

	"--- identities and edges ---"
	t check: 'shifting by zero is identity' equals: 12345 is: [ 12345 bitShift: 0 ].
	t check: 'shifting by zero preserves sign' equals: -12345 is: [ -12345 bitShift: 0 ].
	t check: 'zero shifts to zero, left' equals: 0 is: [ 0 bitShift: 100 ].
	t check: 'zero shifts to zero, right' equals: 0 is: [ 0 bitShift: -100 ].
	t check: 'maxVal survives a right shift' equals: SmallInteger maxVal // 2
		is: [ SmallInteger maxVal bitShift: -1 ].
	t check: 'minVal survives a right shift' equals: SmallInteger minVal // 2
		is: [ SmallInteger minVal bitShift: -1 ].
	t check: 'left then right is identity when nothing was lost' equals: 12345
		is: [ (12345 bitShift: 30) bitShift: -30 ].

	"--- LargeInteger receivers ---"
	t check: 'LargeInteger right shift' equals: (2 raisedTo: 99)
		is: [ (2 raisedTo: 100) bitShift: -1 ].
	t check: 'LargeInteger left shift' equals: (2 raisedTo: 101)
		is: [ (2 raisedTo: 100) bitShift: 1 ].
	t check: 'negative LargeInteger right shift floors' equals: (2 raisedTo: 99) negated
		is: [ (2 raisedTo: 100) negated bitShift: -1 ].
	t check: 'LargeInteger shifted all the way down' equals: 0
		is: [ (2 raisedTo: 100) bitShift: -200 ].
	t check: 'negative LargeInteger shifted all the way down floors to -1' equals: -1
		is: [ (2 raisedTo: 100) negated bitShift: -200 ].

	"--- a non-Integer count is still a clean error, not a wrong answer ---"
	t expect: 'a Float shift count raises' raises: Error in: [ 1 bitShift: 2.5 ].
	t expect: 'a String shift count raises' raises: Error in: [ 1 bitShift: 'x' ].

	^t report
]
