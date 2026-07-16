"Regression test: the identityHash primitive must read the object header's
 hash field (a uint32_t at byte offset 8), not one byte past it.

 The ppc64 (big-endian) backend shipped generateHashPrimitive with the tagged
 receiver's displacement one too high, so it read obj+9 instead of obj+8. The
 whole suite still passed, because a wrong-but-DETERMINISTIC hash behaves like
 a hash: it just returned (hash << 8), silently dropping the top 8 bits of
 entropy and disagreeing with the value the C runtime sees in obj->hash. It was
 found only while porting that site to ppc64le, and fixed by using
 varOffset(RawObject, hash) instead of a hand-computed literal.

 The falsifiable signature: reading one byte high forces the hash's LOW BYTE to
 be permanently zero, because that byte is the header's `unused` field. Pre-fix
 the ppc64 BE backend yields exactly 1 distinct low byte here; x64, ppc64le and
 the fixed ppc64 all yield many (41 for this allocation pattern).

 This test is arch-neutral on purpose: it guards the whole class of
 hardcoded-header-offset bugs, which no other test in the suite can see.

 Note the ^failures: an Assert failure alone would print a backtrace and still
 exit 0, so the runner would report this file as a pass."
[
	| lows failures probe first unstable |
	failures := 0.

	"1. the low byte must VARY across freshly allocated objects"
	lows := Set new.
	1 to: 200 do: [:i | lows add: (Object new hash \\ 256) ].
	lows size > 1
		ifFalse: [
			failures := failures + 1.
			Transcript nextPutAll: 'HashPrimitiveTest: FAIL - hash low byte is constant ('
				, lows size printString
				, ' distinct value), the primitive is reading past the hash field'; lf ].

	"2. the hash must be STABLE for a given object, not recomputed per send.
	 Counted separately: reusing the running total here would misreport check 1."
	probe := Object new.
	first := probe hash.
	unstable := 0.
	1 to: 50 do: [:i |
		probe hash = first
			ifFalse: [ unstable := unstable + 1 ] ].
	unstable > 0
		ifTrue: [
			failures := failures + 1.
			Transcript nextPutAll: 'HashPrimitiveTest: FAIL - hash not stable per object ('
				, unstable printString , ' of 50 reads differed)'; lf ].

	"3. the hash must be a non-negative SmallInteger that fits the u32 field"
	(first >= 0 and: [ first < 4294967296 ])
		ifFalse: [
			failures := failures + 1.
			Transcript nextPutAll: 'HashPrimitiveTest: FAIL - hash out of the u32 range'; lf ].

	failures = 0
		ifTrue: [ Transcript nextPutAll: 'HashPrimitiveTest: OK'; lf ].
	^failures
]
