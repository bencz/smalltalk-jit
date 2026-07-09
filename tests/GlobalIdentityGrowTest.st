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

"Pad classes: each installs a global (its own name). The post-bootstrap dict
 sits at ~248/256, so ~20 of these comfortably cross the grow threshold."
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

[
	| fails |
	fails := 0.
	GrowMutable := 42.

	"sanity: the system dictionary really did grow past its initial capacity,
	 so this test actually exercises the rehash path"
	(Smalltalk size > 256) ifFalse: [
		Transcript nextPutAll: 'GrowTest: dict did NOT grow (size=';
			nextPutAll: Smalltalk size printString; nextPutAll: ')'; lf.
		fails := fails + 1 ].

	"the crux: identity preserved => the eagerly-compiled read observes the value
	 the later block wrote (42), not the stale nil"
	((GrowHolder new read) = 42) ifFalse: [
		Transcript nextPutAll: 'GrowTest: stale read=';
			nextPutAll: (GrowHolder new read) printString; nextPutAll: ' (expected 42)'; lf.
		fails := fails + 1 ].

	^fails
]
