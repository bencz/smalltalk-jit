"Tier-1 SPECULATION INVALIDATION, promoted-send variant (jit/Tier.h,
 generateIcSendPromoted).

 A hot monomorphic send whose callee the INLINER rejects still gets PROMOTED:
 the receiver class and the callee entry are baked as immediates, guarded by a
 single compare, with the full IC send as the fallback. The hit path reads NO
 IC cell, so icInvalidateAllSends (which only resets cells) cannot reach it,
 and redefineMutate preserves the class object identity so the guard still
 matches. Without explicit invalidation the site keeps calling the OLD entry.

 The callee here contains CONTROL FLOW (an ifTrue:ifFalse:, compiled to jumps)
 precisely so inlineEligible rejects it and the site is promoted rather than
 inlined. That separates this variant from TierInlineRedefineTest.

 WARNING: the tier counter lives in the METHOD PROLOGUE and there is no OSR,
 so the driver method must be CALLED repeatedly, not looped inside."

TierRedefHot := Object [
	"control flow => not inlinable, but the send site is still promotable"
	classify: n [ n > 0 ifTrue: [ ^1 ] ifFalse: [ ^0 ] ]
]

TierRedefDriver := Object [

	| target |

	setUp [ target := TierRedefHot new. ^self ]

	call [ ^target classify: 5 ]

	class redefineTo: aString [
		Compiler new buildClass:
			(Parser parseString:
				'TierRedefHot := Object [ classify: n [ n > 0 ifTrue: [ ^', aString,
				' ] ifFalse: [ ^0 ] ] ]') parseClass.
	]

]

[ | t d sum |
	t := TestRun named: 'TierRedefine'.
	d := TierRedefDriver new setUp.

	1 to: 20000 do: [:i | d call].
	t check: 'warm site answers the original method' equals: 1 is: [ d call ].

	TierRedefDriver redefineTo: '3'.

	t check: 'a cold direct send sees the redefinition' equals: 3 is: [ TierRedefHot new classify: 5 ].
	t check: 'the WARM promoted site sees the redefinition' equals: 3 is: [ d call ].

	sum := 0.
	1 to: 10 do: [:i | sum := sum + d call].
	t check: '10 further calls all see the redefinition' equals: 30 is: [ sum ].

	t check: 'fresh receiver through the warm site' equals: 3 is: [ TierRedefDriver new setUp call ].

	TierRedefDriver redefineTo: '9'.
	t check: 'the warm site tracks a SECOND redefinition' equals: 9 is: [ d call ].

	^t report
]
