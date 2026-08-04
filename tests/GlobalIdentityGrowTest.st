"Regression (Bug B): the C-level growDictionary must preserve Association object
 identity when the Smalltalk system dictionary rehashes. The JIT bakes a fixed
 Association pointer into compiled code (OPERAND_ASSOC); if a rehash replaces it
 with a freshly allocated association, a global written through the new binding
 reads back nil through the stale one still baked into older code.

 GrowHolder is defined FIRST, so its `read` method eagerly captures GrowMutable's
 association BEFORE the pad classes below push the Smalltalk dict past its 256
 grow threshold. The top-level assignment block is compiled AFTER the grow. Pre-
 fix, writer and reader bound different associations and the read saw nil.

 Returns 0 on success, nonzero on failure (see the harness note in
 BooleanGlobalTest.st for why a caret-returned integer is required to be caught
 by run_tests.sh). This path is a plain read, so no error wrapping is needed."

GrowHolder := Object [
	read [ ^GrowMutable ]
]

"Pad classes: each installs a global (its own name), through the SAME C path a
 class definition always takes, which is the path whose growDictionary is under
 test. How many are needed is a property of the kernel, not of this test: the
 dictionary rehashes at 3/4 of its 256 slots, so it takes as many as the gap
 between 192 and however many globals the bootstrap leaves. v1 left ~248 and 20
 sufficed; v2's kernel is smaller (165), so 20 landed at 187 -- four short, and
 the test reported a failure for a rehash that never had to happen."
GrowPad01 := Object [ ]
GrowPad02 := Object [ ]
GrowPad03 := Object [ ]
GrowPad04 := Object [ ]
GrowPad05 := Object [ ]
GrowPad06 := Object [ ]
GrowPad07 := Object [ ]
GrowPad08 := Object [ ]
GrowPad09 := Object [ ]
GrowPad10 := Object [ ]
GrowPad11 := Object [ ]
GrowPad12 := Object [ ]
GrowPad13 := Object [ ]
GrowPad14 := Object [ ]
GrowPad15 := Object [ ]
GrowPad16 := Object [ ]
GrowPad17 := Object [ ]
GrowPad18 := Object [ ]
GrowPad19 := Object [ ]
GrowPad20 := Object [ ]
GrowPad21 := Object [ ]
GrowPad22 := Object [ ]
GrowPad23 := Object [ ]
GrowPad24 := Object [ ]
GrowPad25 := Object [ ]
GrowPad26 := Object [ ]
GrowPad27 := Object [ ]
GrowPad28 := Object [ ]
GrowPad29 := Object [ ]
GrowPad30 := Object [ ]
GrowPad31 := Object [ ]
GrowPad32 := Object [ ]
GrowPad33 := Object [ ]
GrowPad34 := Object [ ]
GrowPad35 := Object [ ]
GrowPad36 := Object [ ]
GrowPad37 := Object [ ]
GrowPad38 := Object [ ]
GrowPad39 := Object [ ]
GrowPad40 := Object [ ]

[
	| fails |
	fails := 0.
	GrowMutable := 42.

	"sanity: the system dictionary really did REHASH, so this test exercises the
	 path it is about. The CONTENTS ARRAY is what says so -- it is 256 slots out
	 of the bootstrap and doubles on the first grow -- and the number of globals
	 is not: that only says how many the kernel happens to have."
	((Smalltalk instVarAt: 1) size > 256) ifFalse: [
		Transcript nextPutAll: 'GrowTest: dict did NOT rehash (entries=';
			nextPutAll: Smalltalk size printString; nextPutAll: ' slots=';
			nextPutAll: (Smalltalk instVarAt: 1) size printString; nextPutAll: ')'; lf.
		fails := fails + 1 ].

	"the crux: identity preserved => the eagerly-compiled read observes the value
	 the later block wrote (42), not the stale nil"
	((GrowHolder new read) = 42) ifFalse: [
		Transcript nextPutAll: 'GrowTest: stale read=';
			nextPutAll: (GrowHolder new read) printString; nextPutAll: ' (expected 42)'; lf.
		fails := fails + 1 ].

	^fails
]
