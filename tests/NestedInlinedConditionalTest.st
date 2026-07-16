"Regression: deeply nested inlined conditionals.

 ifTrue:/ifFalse:/and:/or: with literal arg-free blocks are spliced into the
 enclosing frame instead of being sent. Each level keeps its condition live
 across everything below it (the #mustBeBoolean arm re-reads the receiver after
 the whole false arm), so about 5 levels of nesting exhaust the JIT register
 pool and the allocator starts SPILLING.

 A spilled variable owns no register: its reg is SPILLED_REG (-1). The x64
 class-check and copy paths used to hand that -1 straight to the emitter as a
 register NUMBER, which encodes a wild register, so the conditional tested
 garbage: #mustBeBoolean on a live receiver, or a SIGSEGV on a non-canonical
 address. POWER never had this (see varReg() in CodeGeneratorPpc64.c); the fix
 is fillVarToReg() in CodeGeneratorX64.c.

 Found by benchmarks/Richards.st, whose task bodies nest this deep.

 Every condition below is false, so control MUST reach the innermost arm and
 answer 'BOTTOM'. Any other answer is a miscompile. Run with ST_NO_INLINE_CF=1
 to compare against the non-inlined path."

NestedCond := Object [

	"Plain method: no sends in the conditions, so this isolates nesting depth
	 from register pressure caused by anything else."
	class depth5 [
		| c0 c1 c2 c3 c4 |
		c0 := false. c1 := false. c2 := false. c3 := false. c4 := false.
		^c0 ifTrue: ['T0']
			ifFalse: [c1 ifTrue: ['T1']
				ifFalse: [c2 ifTrue: ['T2']
					ifFalse: [c3 ifTrue: ['T3']
						ifFalse: [c4 ifTrue: ['T4']
							ifFalse: ['BOTTOM']]]]]
	]


	class depth8 [
		| c0 c1 c2 c3 c4 c5 c6 c7 |
		c0 := false. c1 := false. c2 := false. c3 := false.
		c4 := false. c5 := false. c6 := false. c7 := false.
		^c0 ifTrue: ['T0']
			ifFalse: [c1 ifTrue: ['T1']
				ifFalse: [c2 ifTrue: ['T2']
					ifFalse: [c3 ifTrue: ['T3']
						ifFalse: [c4 ifTrue: ['T4']
							ifFalse: [c5 ifTrue: ['T5']
								ifFalse: [c6 ifTrue: ['T6']
									ifFalse: [c7 ifTrue: ['T7']
										ifFalse: ['BOTTOM']]]]]]]]
	]


	"Conditions computed by sends, and statements in the arms: this is the shape
	 Richards' task bodies actually have."
	class depth5WithSends: anObject [
		| a b c d e |
		a := anObject. b := anObject. c := anObject. d := anObject. e := anObject.
		^a isNil ifTrue: ['T0']
			ifFalse: [b isNil ifTrue: ['T1']
				ifFalse: [c isNil ifTrue: ['T2']
					ifFalse: [d isNil ifTrue: ['T3']
						ifFalse: [e isNil ifTrue: ['T4']
							ifFalse: [
								a size.
								b size.
								'BOTTOM']]]]]
	]


	"Same nesting inside a real block (a closure with args and temps), which is
	 how Richards stores its task functions."
	class depth5InBlock [
		^[:x :y |
			| a b c d e |
			a := x. b := x. c := x. d := y. e := y.
			a isNil ifTrue: ['T0']
				ifFalse: [b isNil ifTrue: ['T1']
					ifFalse: [c isNil ifTrue: ['T2']
						ifFalse: [d isNil ifTrue: ['T3']
							ifFalse: [e isNil ifTrue: ['T4']
								ifFalse: [
									a size.
									b size.
									'BOTTOM']]]]]]
	]


	"and:/or: are spliced by the same code path."
	class depth5AndOr [
		| a b c d e |
		a := false. b := false. c := false. d := false. e := false.
		^(a or: [b or: [c or: [d or: [e]]]])
			ifTrue: ['T0']
			ifFalse: [(a not and: [b not and: [c not and: [d not and: [e not]]]])
				ifTrue: ['BOTTOM']
				ifFalse: ['T1']]
	]
]

[
	| fails check |
	fails := 0.

	"An uncaught #mustBeBoolean would kill the fiber while main.c still exits 0,
	 which run_tests.sh reads as a pass, so catch it and count it instead."
	check := [:name :block |
		[block value = 'BOTTOM'
			ifFalse: [
				fails := fails + 1.
				Transcript nextPutAll: 'FAIL (wrong value): '; nextPutAll: name; lf]]
			on: Error
			do: [:e |
				fails := fails + 1.
				Transcript nextPutAll: 'FAIL (raised): '; nextPutAll: name; lf]].

	check value: 'depth5' value: [NestedCond depth5].
	check value: 'depth8' value: [NestedCond depth8].
	check value: 'depth5WithSends' value: [NestedCond depth5WithSends: 'abc'].
	check value: 'depth5InBlock' value: [NestedCond depth5InBlock value: 'abc' value: 'de'].
	check value: 'depth5AndOr' value: [NestedCond depth5AndOr].

	fails = 0 ifTrue: [Transcript nextPutAll: 'NestedInlinedConditionalTest: OK'; lf].
	^fails
]
