"Tier-1 guard-fail correctness (jit/Tier.h): a send site promoted to an
 exact-class direct call receives receivers of NEW classes only AFTER the
 promotion. The baked guard must miss for every one of them and the IC
 fallback must dispatch each override correctly; the guard is never rebound
 (the promoted class keeps its direct path). A wrong answer from any class is
 a counted failure."

TierGfBase := Object [
	tag [ ^1 ]
	probe [ ^self tag + 10 ]
]
TierGfSub := TierGfBase [ tag [ ^2 ] ]
TierGfOther := TierGfBase [ tag [ ^5 ] ]

[ | fails base sub other |
	fails := 0.
	base := TierGfBase new.
	sub := TierGfSub new.
	other := TierGfOther new.

	"Heat probe with ONLY the base class past the threshold: at recompile time
	 the `self tag` site is monomorphic TierGfBase and gets promoted."
	1 to: 2500 do: [:i |
		base probe = 11 ifFalse: [fails := fails + 1]].

	"Churn so the driver's sites re-miss and adopt the recompiled probe."
	1 to: 10000 do: [:i | Array new: 1024].
	1 to: 100 do: [:i |
		base probe = 11 ifFalse: [fails := fails + 1]].

	"probe is INHERITED by the subclasses: the same tier-1 code runs with self
	 of another exact class, so the promoted guard must miss and the fallback
	 send must find each override, while the base keeps its direct path."
	1 to: 300 do: [:i |
		sub probe = 12 ifFalse: [fails := fails + 1].
		other probe = 15 ifFalse: [fails := fails + 1].
		base probe = 11 ifFalse: [fails := fails + 1]].

	fails = 0 ifTrue: [Transcript nextPutAll: 'TierGuardFailTest: OK'; lf].
	^fails
]
