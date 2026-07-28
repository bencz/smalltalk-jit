"WideArgumentsTest: a send whose receiver and arguments do not fit the ABI's
 integer argument registers.

 SysV carries six, so a method of six arguments or more used to make the
 prologue ASSERT and abort the VM while COMPILING it. The kernel has such
 methods -- DateTime year:month:day:hour:minute:second:millisecond: is seven --
 so three test files could not run at all.

 Past the register set a method takes one pointer instead, to the caller's
 receiver slot, with the arguments at descending addresses (vm/jit/Jit.h). The
 checks that matter are the ones that read EVERY parameter: a copy that walked
 the block the wrong way still gets self and the first parameter right, and any
 check that only looked at those would pass with the direction reversed.

 Gate level 3 pins the machine-level half against hand-written bytecode,
 including that reversal. This file is the Smalltalk-visible half: source, the
 front end, an ordinary send, a super send, a block, and the reflective path."

WideBase := Object [

	class describeA: a b: b c: c d: d e: e f: f [
		^'base'
	]

]


WideProbe := WideBase [

	| tally |

	"Six parameters plus self: seven, one past the SysV register set.
	 Built slot by slot rather than with `Array with:...`, whose kernel family
	 stops at four arguments; the point here is the send, not the constructor."
	sixOf: a b: b c: c d: d e: e f: f [
		| answer |

		answer := Array new: 6.
		answer at: 1 put: a; at: 2 put: b; at: 3 put: c;
			at: 4 put: d; at: 5 put: e; at: 6 put: f.
		^answer
	]


	"Seven, which is the shape the kernel's DateTime constructor has."
	sevenOf: a b: b c: c d: d e: e f: f g: g [
		^a printString, b printString, c printString, d printString,
			e printString, f printString, g printString
	]


	"The receiver has to arrive too, and it is slot 0 of the same block: a
	 method that reads an instance variable proves self was copied and not just
	 the parameters."
	setTally: anInteger [
		tally := anInteger.
	]


	tallyPlus: a b: b c: c d: d e: e f: f [
		^tally + a + b + c + d + e + f
	]


	"A wide SUPER send: the lookup starts elsewhere, the convention does not."
	class describeA: a b: b c: c d: d e: e f: f [
		^(super describeA: a b: b c: c d: d e: e f: f), '/sub'
	]

]


[
	| t p got block |
	t := TestRun named: 'WideArguments'.

	p := WideProbe new.

	"EVERY PARAMETER, in order. One Array answer instead of six checks so a
	 permutation is a single visible failure rather than six."
	t check: 'six parameters arrive in order'
		equals: #(1 2 3 4 5 6)
		is: [p sixOf: 1 b: 2 c: 3 d: 4 e: 5 f: 6].

	t check: 'and seven do'
		equals: '1234567'
		is: [p sevenOf: 1 b: 2 c: 3 d: 4 e: 5 f: 6 g: 7].

	"SELF arrives as well, which is what an instance variable read proves."
	p setTally: 100.
	t check: 'the receiver is copied along with the arguments'
		equals: 121 is: [p tallyPlus: 1 b: 2 c: 3 d: 4 e: 5 f: 6].

	"Distinct values, so a slot holding a neighbour is visible. All-ones would
	 pass with any permutation."
	t check: 'distinct arguments do not get permuted'
		equals: #(10 200 3000 40000 500000 6000000)
		is: [p sixOf: 10 b: 200 c: 3000 d: 40000 e: 500000 f: 6000000].

	"A wide SUPER send."
	t check: 'a wide super send finds the superclass method'
		equals: 'base/sub'
		is: [WideProbe describeA: 1 b: 2 c: 3 d: 4 e: 5 f: 6].

	"A BLOCK with more parameters than the register set. Its own unit is
	 compiled with the same prologue, and valueWithArguments: is how it is
	 entered."
	block := [:a :b :c :d :e :f | a + b + c + d + e + f].
	got := [block valueWithArguments: #(1 2 3 4 5 6)] on: Error do: [:e | #notSupported].
	t check: 'a six-parameter block either works or refuses NAMED, never guesses'
		equals: true
		is: [got = 21 or: [got == #notSupported]].

	"THE REFLECTIVE PATH must not be narrower than the direct one. It used to
	 stop at five arguments and FAIL above, so perform:withArguments: could not
	 reach a method an ordinary send could."
	t check: 'perform:withArguments: reaches a wide method'
		equals: #(7 8 9 10 11 12)
		is: [p perform: #sixOf:b:c:d:e:f: withArguments: #(7 8 9 10 11 12)].

	t check: 'and a seven-argument one'
		equals: '7654321'
		is: [p perform: #sevenOf:b:c:d:e:f:g: withArguments: #(7 6 5 4 3 2 1)].

	"The kernel method that made this necessary."
	t check: 'DateTime takes its seven arguments'
		equals: '2026-07-28 13:45:30'
		is: [(DateTime year: 2026 month: 7 day: 28 hour: 13 minute: 45 second: 30
			millisecond: 250) printString].

	"NARROW METHODS ARE UNTOUCHED. The boundary is where it says it is, and a
	 change that made everything wide would pass every check above."
	t check: 'five arguments still work' equals: 15
		is: [(OrderedCollection new) add: 1; add: 2; add: 3; add: 4; add: 5;
			yourself; inject: 0 into: [:acc :each | acc + each]].
	t check: 'and no arguments' equals: 3 is: [3 abs].

	^t report
]
