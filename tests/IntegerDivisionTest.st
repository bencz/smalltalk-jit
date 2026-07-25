"Identity test for the integer division family: // \\ quo: rem:

 Checked by IDENTITIES rather than by a table of expected answers, because the
 interesting cases are the sign combinations and the range boundary, and a table
 of those is exactly where a hand-written expectation is as likely to be wrong as
 the code. The identities below define floored and truncated division completely,
 so anything that satisfies all of them is correct.

 What this exists to pin:
   * // was added as a primitive (IntFloorDivPrimitive); before that SmallInteger
     had no // at all and inherited the Smalltalk Integer>>//.
   * quo: silently answered minVal for `minVal quo: -1`, whose real answer
     (+2^61) does not fit a SmallInteger. Both // and quo: now refuse it and let
     the fallback promote.
   * \\ must floor (sign follows the DIVISOR) while rem: truncates (sign follows
     the DIVIDEND). Getting those two backwards still looks plausible.

 Harness note (see BooleanGlobalTest.st): an uncaught error still exits 0, so
 this counts failures and ^-returns the count."

[
	| fails check operands |
	fails := 0.

	check := [ :a :b |
		| q r tq tr |
		q := a // b.
		r := a \\ b.
		tq := a quo: b.
		tr := a rem: b.

		"1. Division identity, for both flavours. If this holds and the
		 remainder is in range, the quotient is forced."
		(q * b + r) = a ifFalse: [ fails := fails + 1.
			Transcript nextPutAll: '// identity broke: ', a printString, ' // ', b printString; lf ].
		(tq * b + tr) = a ifFalse: [ fails := fails + 1.
			Transcript nextPutAll: 'quo: identity broke: ', a printString, ' quo: ', b printString; lf ].

		"2. Magnitude: a remainder at or beyond the divisor means the quotient
		 was off by at least one."
		(r abs < b abs) ifFalse: [ fails := fails + 1.
			Transcript nextPutAll: '\\ out of range: ', a printString, ' \\ ', b printString, ' = ', r printString; lf ].
		(tr abs < b abs) ifFalse: [ fails := fails + 1.
			Transcript nextPutAll: 'rem: out of range: ', a printString, ' rem: ', b printString, ' = ', tr printString; lf ].

		"3. Sign rules, which are what separate the two families."
		(r = 0 or: [ (r < 0) = (b < 0) ]) ifFalse: [ fails := fails + 1.
			Transcript nextPutAll: '\\ sign must follow the divisor: ', a printString, ' \\ ', b printString, ' = ', r printString; lf ].
		(tr = 0 or: [ (tr < 0) = (a < 0) ]) ifFalse: [ fails := fails + 1.
			Transcript nextPutAll: 'rem: sign must follow the dividend: ', a printString, ' rem: ', b printString, ' = ', tr printString; lf ].

		"4. Floored vs truncated agree exactly when the division is exact or the
		 signs match; otherwise the floored quotient is one lower."
		((r = 0 or: [ (a < 0) = (b < 0) ])
			ifTrue: [ q = tq ]
			ifFalse: [ q = (tq - 1) ]) ifFalse: [ fails := fails + 1.
				Transcript nextPutAll: '// vs quo: disagree: ', a printString, ' , ', b printString; lf ] ].

	"All four sign combinations across small magnitudes, including the exact
	 divisions where the floor correction must NOT fire."
	-40 to: 40 do: [ :a |
		-12 to: 12 do: [ :b | b = 0 ifFalse: [ check value: a value: b ] ] ].

	"Divisor 1 and -1: the quotient keeps the dividend's magnitude, which is
	 where the tagged representation runs out of room."
	#(1 -1 2 -2 3 -3 7 -7 1000 -1000 65536 -65536) do: [ :b |
		#(0 1 -1 7 -7 100 -100 99999 -99999 1073741824 -1073741824) do: [ :a |
			check value: a value: b ] ].

	"The range boundary. minVal // -1 and minVal quo: -1 are +2^61, one past
	 SmallInteger, so they must PROMOTE rather than wrap. The wrap used to
	 answer minVal itself, a silent wrong answer."
	(SmallInteger minVal // -1) = 2305843009213693952 ifFalse: [ fails := fails + 1.
		Transcript nextPutAll: 'minVal // -1 wrong: ', (SmallInteger minVal // -1) printString; lf ].
	(SmallInteger minVal quo: -1) = 2305843009213693952 ifFalse: [ fails := fails + 1.
		Transcript nextPutAll: 'minVal quo: -1 wrong: ', (SmallInteger minVal quo: -1) printString; lf ].
	check value: SmallInteger minVal value: 1.
	check value: SmallInteger minVal value: 2.
	check value: SmallInteger minVal value: -2.
	check value: SmallInteger maxVal value: 1.
	check value: SmallInteger maxVal value: -1.
	check value: SmallInteger maxVal value: 2.
	check value: SmallInteger maxVal value: -2.

	"Division by zero must still raise, for all four."
	([ 1 // 0. false ] on: ZeroDivide do: [ :e | true ]) ifFalse: [ fails := fails + 1 ].
	([ 1 \\ 0. false ] on: ZeroDivide do: [ :e | true ]) ifFalse: [ fails := fails + 1 ].
	([ 1 quo: 0. false ] on: ZeroDivide do: [ :e | true ]) ifFalse: [ fails := fails + 1 ].
	([ 1 rem: 0. false ] on: ZeroDivide do: [ :e | true ]) ifFalse: [ fails := fails + 1 ].

	"A non-integer divisor must still coerce rather than hit the primitive."
	((7 // 2.0) = 3) ifFalse: [ fails := fails + 1.
		Transcript nextPutAll: '7 // 2.0 = ', (7 // 2.0) printString; lf ].

	fails > 0 ifTrue: [
		Transcript nextPutAll: 'IntegerDivisionTest FAILURES='; nextPutAll: fails printString; lf ].
	^fails
]
