"Tier-1 SPECULATION INVALIDATION, inlined-body variant (compiler/Optimizer.c,
 core/Class.c installClass).

 A hot send whose monomorphic callee is eligible gets the callee's BYTECODES
 COPIED into the caller behind a JUMP_NOT_MEMBER_OF exact-class guard. Class
 redefinition then copies the new raw class OVER the old object (redefineMutate
 memcpy), so the class OBJECT IDENTITY survives and the guard still matches.
 icInvalidateAllSends resets every IC cell, but an inlined body reads no cell
 and IS the stale method: without explicit invalidation the site keeps running
 the OLD code forever.

 WARNING on test shape: the tier counter is decremented in the METHOD PROLOGUE
 only and there is no OSR, so a loop INSIDE one call never reaches tier 1. The
 driver method must be CALLED repeatedly. Run with a low ST_TIER_THRESHOLD to
 keep this fast; the default 1000 also works, it just needs more calls."

TierInlRedefHot := Object [
	probe [ ^1 ]
]

TierInlRedefDriver := Object [

	| target |

	setUp [ target := TierInlRedefHot new. ^self ]

	"one send per call, so repeated CALLS of this method heat the tier counter"
	call [ ^target probe ]

	class redefineTo: aString [
		Compiler new buildClass:
			(Parser parseString: 'TierInlRedefHot := Object [ probe [ ^', aString, ' ] ]') parseClass.
	]

]

[ | t d sum |
	t := TestRun named: 'TierInlineRedefine'.
	d := TierInlRedefDriver new setUp.

	"Phase 1: heat `call` past the tier threshold so `target probe` is inlined."
	1 to: 20000 do: [:i | d call].
	t check: 'warm site answers the original method' equals: 1 is: [ d call ].

	"Phase 2: redefine the callee through the in-image compiler."
	TierInlRedefDriver redefineTo: '2'.

	t check: 'a cold direct send sees the redefinition' equals: 2 is: [ TierInlRedefHot new probe ].
	t check: 'the WARM inlined site sees the redefinition' equals: 2 is: [ d call ].

	sum := 0.
	1 to: 10 do: [:i | sum := sum + d call].
	t check: '10 further calls all see the redefinition' equals: 20 is: [ sum ].

	"Phase 3: a receiver created AFTER the redefinition, through the same warm
	 site, must also see the new method."
	t check: 'fresh receiver through the warm site' equals: 2 is: [ TierInlRedefDriver new setUp call ].

	"Phase 4: redefine a second time, so the fix cannot be a one-shot."
	TierInlRedefDriver redefineTo: '7'.
	t check: 'the warm site tracks a SECOND redefinition' equals: 7 is: [ d call ].

	^t report
]
