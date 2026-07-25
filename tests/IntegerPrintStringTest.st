"Differential test for IntAsStringBasePrimitive against the Smalltalk body it
 shortcuts (SmallInteger>>printStringBase:).

 A formatting primitive is exactly the kind of change whose bugs are invisible
 in aggregate: a wrong digit in one base, or a sign dropped on the most negative
 value, still prints something that LOOKS like a number. So this does not check
 a handful of expected strings, it re-derives every answer from the definition
 and compares.

 The reference implementation is kept in a separate class rather than by
 disabling the primitive, because a primitive that is present cannot be turned
 off per-call.

 Harness note (see BooleanGlobalTest.st): an uncaught error still exits 0, so
 this counts failures and ^-returns the count."

IntPrintRef := Object [
	"The pre-primitive Smalltalk algorithm, verbatim."
	printStringBase: aBase of: anInteger [
		| int string i |
		anInteger < 0
			ifTrue: [
				int := anInteger negated.
				i := (int floorLog: aBase) + 2.
				string := String new: i.
				string at: 1 put: $-]
			ifFalse: [
				int := anInteger.
				i := (int floorLog: aBase) + 1.
				string := String new: i].
		[string at: i put: (Character digitValue: int \\ aBase).
		i := i - 1.
		(int := int // aBase) > 0]
			whileTrue.
		^string
	]
]

[
	| fails ref check values |
	fails := 0.
	ref := IntPrintRef new.

	check := [ :n :base |
		| got want |
		got := [n printStringBase: base] on: Error do: [ :e | 'RAISED' ].
		want := [ref printStringBase: base of: n] on: Error do: [ :e | 'RAISED' ].
		got = want ifFalse: [
			fails := fails + 1.
			Transcript nextPutAll: 'MISMATCH n=', n printString, ' base=', base printString,
				' primitive=', got printString, ' reference=', want printString; lf ] ].

	"Small values across every supported base: this is where a digit-table or an
	 off-by-one in the uppercase range shows up."
	2 to: 36 do: [ :base |
		0 to: 200 do: [ :n | check value: n value: base. check value: n negated value: base ] ].

	"Powers and neighbours in each base: the carry boundaries, where a digit
	 count computed one way and filled another disagree."
	2 to: 36 do: [ :base |
		| p |
		p := base.
		[ p < 1000000000 ] whileTrue: [
			check value: p - 1 value: base.
			check value: p value: base.
			check value: p + 1 value: base.
			check value: p negated value: base.
			p := p * base ] ].

	"Assorted magnitudes in decimal, the base that actually runs in production."
	values := #(7 42 99 100 101 999 1000 1001 65535 65536 1000000 999999999
	            1073741823 1073741824 2147483647 2147483648 4294967295 4294967296).
	values do: [ :n | check value: n value: 10. check value: n negated value: 10 ].

	"maxVal is an ordinary differential case."
	check value: SmallInteger maxVal value: 10.
	check value: SmallInteger maxVal value: 16.
	check value: SmallInteger maxVal value: 2.

	"minVal is NOT, and is asserted against the arithmetic rather than against
	 the reference, because the reference is wrong there. The most negative
	 SmallInteger has no positive counterpart in its own range, and
	 `SmallInteger minVal negated` raises 'Integer overflow' instead of
	 promoting to a LargePositiveInteger, so the old printStringBase: could not
	 print the smallest number in its own class. The primitive formats from the
	 negative side and gets it right; this test pins that, so a future rewrite
	 cannot quietly reintroduce the raise."
	(SmallInteger minVal printString = '-2305843009213693952')
		ifFalse: [ fails := fails + 1.
			Transcript nextPutAll: 'minVal decimal wrong: ', SmallInteger minVal printString; lf ].
	((SmallInteger minVal printStringBase: 16) = '-2000000000000000')
		ifFalse: [ fails := fails + 1.
			Transcript nextPutAll: 'minVal hex wrong: ', (SmallInteger minVal printStringBase: 16); lf ].
	"Base-independent check without needing a parser: re-derive the value from
	 the digits by Horner, and it must come back to minVal. This catches a wrong
	 digit in any base, which the two literals above only catch in 10 and 16."
	2 to: 36 do: [ :base |
		| s acc digits ok |
		s := SmallInteger minVal printStringBase: base.
		ok := (s at: 1) = $-.
		digits := '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'.
		acc := 0.
		2 to: s size do: [ :k |
			| d |
			d := (digits indexOf: (s at: k)) - 1.
			(d < 0 or: [ d >= base ]) ifTrue: [ ok := false ].
			acc := acc * base + d ].
		(ok and: [ acc negated = SmallInteger minVal ]) ifFalse: [
			fails := fails + 1.
			Transcript nextPutAll: 'minVal digits wrong in base ', base printString, ': ', s; lf ] ].

	"printString is printStringBase: 10, and must agree."
	(values , #(0 1)) do: [ :n |
		n printString = (n printStringBase: 10) ifFalse: [ fails := fails + 1 ].
		n negated printString = (n negated printStringBase: 10) ifFalse: [ fails := fails + 1 ] ].

	"A base the primitive must REFUSE, falling through to the Smalltalk body,
	 which raises. Both sides must behave the same."
	check value: 10 value: 1.
	check value: 10 value: 37.
	check value: 10 value: 0.

	fails > 0 ifTrue: [
		Transcript nextPutAll: 'IntegerPrintStringTest FAILURES='; nextPutAll: fails printString; lf ].
	^fails
]
