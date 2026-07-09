"Regression: the JIT call-site Float intrinsic (inlined + - * / and < <= > >= = ~=
 in generateSend, backed by the SSE emitters) must match the C-primitive results,
 fall back to coercion for mixed int/float, follow IEEE/NaN semantics, and box
 arithmetic results GC-safely under allocation pressure.

 Returns 0 on success, nonzero on failure (an Assert failure only prints and still
 exits 0 in this VM, so a caret-returned count is what run_tests.sh can catch — see
 the harness note in BooleanGlobalTest.st). Arithmetic is checked via printString
 (the C printing path, independent of the arithmetic intrinsic) so a broken intrinsic
 cannot mask itself; comparisons are the boolean under test directly. Cross-check the
 intrinsic against the C path with ST_NO_INLINE_FLOAT=1 (must give identical output)."

[
	| fails s |
	fails := 0.

	"arithmetic (exact binary results, verified through printString)"
	((2.5 + 1.25) printString = '3.75') ifFalse: [ fails := fails + 1 ].
	((10.0 - 3.5) printString = '6.5')  ifFalse: [ fails := fails + 1 ].
	((2.0 * 3.5) printString = '7.0')   ifFalse: [ fails := fails + 1 ].
	((7.0 / 2.0) printString = '3.5')   ifFalse: [ fails := fails + 1 ].

	"comparisons (the result IS the boolean under test)"
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

	"mixed int/float must coerce (fall through the intrinsic to retry:coercing:)"
	((2.5 + 3) printString = '5.5') ifFalse: [ fails := fails + 1 ].
	((3 + 2.5) printString = '5.5') ifFalse: [ fails := fails + 1 ].
	(2.0 = 2) ifFalse: [ fails := fails + 1 ].
	(3 < 2.5) ifTrue:  [ fails := fails + 1 ].

	"NaN / inf: unordered compares are false (except ~= which is true)"
	((0.0 / 0.0) < 1.0)          ifTrue:  [ fails := fails + 1 ].
	((0.0 / 0.0) = (0.0 / 0.0))  ifTrue:  [ fails := fails + 1 ].
	((0.0 / 0.0) ~= 1.0)         ifFalse: [ fails := fails + 1 ].
	((1.0 / 0.0) > 1000000.0)    ifFalse: [ fails := fails + 1 ].

	"hot loop: 100k boxed additions exercise the allocate-first-compute-after path
	 under real GC pressure; the sum must be exact"
	s := 0.0.
	1 to: 100000 do: [:i | s := s + 1.5].
	(s printString = '150000.0') ifFalse: [ fails := fails + 1 ].

	fails > 0 ifTrue: [
		Transcript nextPutAll: 'FloatIntrinsicTest FAILURES='; nextPutAll: fails printString; lf ].
	^fails
]
