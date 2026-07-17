"Regression: the JIT call-site Float intrinsic (inlined + - * / and < <= > >= = ~=
 in generateSend, backed by the SSE emitters) must match the C-primitive results,
 fall back to coercion for mixed int/float, follow IEEE/NaN semantics, box
 arithmetic results GC-safely under allocation pressure, and specialize literal
 float operands (receiver and argument) without changing any result.

 Returns 0 on success, nonzero on failure (an Assert failure only prints and still
 exits 0 in this VM, so a caret-returned count is what run_tests.sh can catch: see
 the harness note in BooleanGlobalTest.st). Arithmetic is checked via printString
 (the C printing path, independent of the arithmetic intrinsic) so a broken intrinsic
 cannot mask itself; comparisons are the boolean under test directly. Cross-check the
 intrinsic against the C path with ST_NO_INLINE_FLOAT=1 (must give identical output).
 Each section is a class-side method so no single compiled method nears the JIT's
 pointer-sites-per-method ceiling (see PORTING.md)."

FloatIntrinsicTest := Object [

	class arithmetic [
		"exact binary results, verified through printString; these constant
		 sends are BOTH-literal shapes: the JIT emits no guard at all"
		| fails |
		fails := 0.
		((2.5 + 1.25) printString = '3.75') ifFalse: [ fails := fails + 1 ].
		((10.0 - 3.5) printString = '6.5')  ifFalse: [ fails := fails + 1 ].
		((2.0 * 3.5) printString = '7.0')   ifFalse: [ fails := fails + 1 ].
		((7.0 / 2.0) printString = '3.5')   ifFalse: [ fails := fails + 1 ].
		^fails
	]


	class comparisons [
		"the result IS the boolean under test"
		| fails |
		fails := 0.
		(2.5 < 3.0)  ifFalse: [ fails := fails + 1 ].
		(3.0 < 2.5)  ifTrue:  [ fails := fails + 1 ].
		(2.0 <= 2.0) ifFalse: [ fails := fails + 1 ].
		(3.0 <= 2.0) ifTrue:  [ fails := fails + 1 ].
		(3.0 > 2.0)  ifFalse: [ fails := fails + 1 ].
		(2.0 >= 2.0) ifFalse: [ fails := fails + 1 ].
		(2.0 = 2.0)  ifFalse: [ fails := fails + 1 ].
		(2.0 = 3.0)  ifTrue:  [ fails := fails + 1 ].
		(2.0 ~= 3.0) ifFalse: [ fails := fails + 1 ].
		(2.0 ~= 2.0) ifTrue:  [ fails := fails + 1 ].
		^fails
	]


	class mixedCoercion [
		"mixed int/float must coerce (fall through the intrinsic to retry:coercing:)"
		| fails |
		fails := 0.
		((2.5 + 3) printString = '5.5') ifFalse: [ fails := fails + 1 ].
		((3 + 2.5) printString = '5.5') ifFalse: [ fails := fails + 1 ].
		(2.0 = 2) ifFalse: [ fails := fails + 1 ].
		(3 < 2.5) ifTrue:  [ fails := fails + 1 ].
		^fails
	]


	class nanAndInf [
		"unordered compares are false (except ~= which is true)"
		| fails |
		fails := 0.
		((0.0 / 0.0) < 1.0)          ifTrue:  [ fails := fails + 1 ].
		((0.0 / 0.0) = (0.0 / 0.0))  ifTrue:  [ fails := fails + 1 ].
		((0.0 / 0.0) ~= 1.0)         ifFalse: [ fails := fails + 1 ].
		((1.0 / 0.0) > 1000000.0)    ifFalse: [ fails := fails + 1 ].
		^fails
	]


	class boxedHotLoop [
		"100k boxed additions exercise the allocate-first-compute-after path
		 under real GC pressure; the sum must be exact"
		| fails s |
		fails := 0.
		s := 0.0.
		1 to: 100000 do: [:i | s := s + 1.5].
		(s printString = '150000.0') ifFalse: [ fails := fails + 1 ].
		^fails
	]


	class receiverLiteral [
		"the JIT specializes a SmallFloat64 literal receiver (no guard, no
		 decode, the constant is materialized), symmetric to the literal
		 argument specialization"
		| fails x |
		fails := 0.
		x := 4.0.
		((1.5 * x) printString = '6.0')  ifFalse: [ fails := fails + 1 ].
		((10.5 - x) printString = '6.5') ifFalse: [ fails := fails + 1 ].
		((14.0 / x) printString = '3.5') ifFalse: [ fails := fails + 1 ].
		((0.5 + x) printString = '4.5')  ifFalse: [ fails := fails + 1 ].
		(1.5 < x)  ifFalse: [ fails := fails + 1 ].
		(4.0 = x)  ifFalse: [ fails := fails + 1 ].
		(1.5 > x)  ifTrue:  [ fails := fails + 1 ].
		(4.0 ~= x) ifTrue:  [ fails := fails + 1 ].
		(1.5 < (0.0 / 0.0))  ifTrue:  [ fails := fails + 1 ].
		(1.5 ~= (0.0 / 0.0)) ifFalse: [ fails := fails + 1 ].
		^fails
	]


	class receiverLiteralCoercion [
		"literal receiver, integer arg: the arg guard must still miss and coerce"
		| fails n |
		fails := 0.
		n := 3.
		((2.5 + n) printString = '5.5') ifFalse: [ fails := fails + 1 ].
		(2.5 < n) ifFalse: [ fails := fails + 1 ].
		^fails
	]


	class literalBoxing [
		"literal operand with an out-of-immediate-range result: the boxing path
		 re-materializes the constant after the allocation stub. Cross-check the
		 receiver-literal, argument-literal, both-literal and no-literal shapes
		 bit-for-bit, then loop the two literal shapes under allocation pressure"
		| fails big s x |
		fails := 0.
		big := 1.0e50.
		s := 1.0e40.
		((1.0e40 * big) = (big * 1.0e40))   ifFalse: [ fails := fails + 1 ].
		((1.0e40 * big) = (s * big))        ifFalse: [ fails := fails + 1 ].
		((1.0e40 * 1.0e50) = (s * big))     ifFalse: [ fails := fails + 1 ].
		1 to: 100000 do: [:i |
			x := 1.0e40 * big.
			(x = (big * 1.0e40)) ifFalse: [ fails := fails + 1 ]].
		^fails
	]


	class runAll [
		| total |
		total := self arithmetic.
		total := total + self comparisons.
		total := total + self mixedCoercion.
		total := total + self nanAndInf.
		total := total + self boxedHotLoop.
		total := total + self receiverLiteral.
		total := total + self receiverLiteralCoercion.
		total := total + self literalBoxing.
		total > 0 ifTrue: [
			Transcript nextPutAll: 'FloatIntrinsicTest FAILURES='; nextPutAll: total printString; lf ].
		^total
	]

]


[
	^FloatIntrinsicTest runAll
]
