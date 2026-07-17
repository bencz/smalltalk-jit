"Unwind through tier-1 code (jit/Tier.h): non-local returns and exceptions
 must keep running ensure: cleanups exactly once per activation when the
 frames they cross belong to RECOMPILED methods. The probe methods get hot
 (crossing the default threshold mid-loop), then churn forces fresh sites to
 rebind to the tier-1 code and the same NLR/raise workload runs again; the
 cleanup counter is asserted exactly (a lost or doubled cleanup miscounts)."

TierUw := Object [
	| log |
	init [ log := 0 ]
	log [ ^log ]
	guarded: aBlock [ ^[ aBlock value ] ensure: [ log := log + 1 ] ]
	nlrProbe: x [ self guarded: [ x > 0 ifTrue: [ ^x * 2 ] ]. ^0 - 1 ]
	raiseProbe: x [ ^[ self guarded: [ Error new signal ] ] on: Error do: [:e | x + 5 ] ]
]

[ | fails u |
	fails := 0.
	u := TierUw new init.

	"Phase 1: cross the threshold with an NLR through ensure: on EVERY
	 iteration; the cleanup must fire exactly once per call, before and after
	 the recompile."
	1 to: 2500 do: [:i |
		(u nlrProbe: i) = (i * 2) ifFalse: [fails := fails + 1]].
	u log = 2500 ifFalse: [fails := fails + 1].

	"Phase 2: churn so fresh misses rebind to the tier-1 code, then drive both
	 the NLR and the exception unwind through it."
	1 to: 10000 do: [:i | Array new: 1024].
	u init.
	1 to: 1500 do: [:i |
		(u nlrProbe: i) = (i * 2) ifFalse: [fails := fails + 1].
		(u raiseProbe: i) = (i + 5) ifFalse: [fails := fails + 1]].
	u log = 3000 ifFalse: [fails := fails + 1].

	fails = 0 ifTrue: [Transcript nextPutAll: 'TierUnwindTest: OK'; lf].
	^fails
]
