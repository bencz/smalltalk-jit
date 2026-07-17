"Inline-cache correctness at ONE dynamic send site (the `tag` send inside
 IcSite class>>through:) driven by a growing set of receiver classes:
 monomorphic, then 2 alternating, then 6 (the planned PIC capacity), then 7
 (one past it, the future mega transition). Every phase asserts the result per
 receiver CLASS, so any wrong-method dispatch (a stale bind, a torn or
 misdirected cache read, a bad mega fallback) turns into a counted failure,
 in every milestone of the IC work and with ST_NO_IC=1 alike."

IcTagA := Object [ tag [ ^1 ] ]
IcTagB := Object [ tag [ ^2 ] ]
IcTagC := Object [ tag [ ^3 ] ]
IcTagD := Object [ tag [ ^4 ] ]
IcTagE := Object [ tag [ ^5 ] ]
IcTagF := Object [ tag [ ^6 ] ]
IcTagG := Object [ tag [ ^7 ] ]

IcSite := Object [
	"The single shared dynamic send site under test."
	class through: anObject [ ^anObject tag ]
]

[ | fails drive |
	fails := 0.

	"drive: send `count` rounds through THE site over `receivers`, asserting the
	 per-class answer on every single send."
	drive := [:receivers :expected :count |
		1 to: count do: [:round |
			1 to: receivers size do: [:k |
				(IcSite through: (receivers at: k)) = (expected at: k)
					ifFalse: [fails := fails + 1]]]].

	"Phase 1: monomorphic warm-up and stability."
	drive value: (Array with: IcTagA new) value: (Array with: 1) value: 2000.

	"Phase 2: 2 classes alternating through the SAME site."
	drive value: (Array with: IcTagA new with: IcTagB new)
		value: (Array with: 1 with: 2) value: 2000.

	"Phase 3: 6 classes round-robin."
	drive value: (OrderedCollection new
			add: IcTagA new; add: IcTagB new; add: IcTagC new;
			add: IcTagD new; add: IcTagE new; add: IcTagF new; yourself)
		value: (OrderedCollection new
			add: 1; add: 2; add: 3; add: 4; add: 5; add: 6; yourself)
		value: 1000.

	"Phase 4: 7 classes, one past the PIC capacity."
	drive value: (OrderedCollection new
			add: IcTagA new; add: IcTagB new; add: IcTagC new; add: IcTagD new;
			add: IcTagE new; add: IcTagF new; add: IcTagG new; yourself)
		value: (OrderedCollection new
			add: 1; add: 2; add: 3; add: 4; add: 5; add: 6; add: 7; yourself)
		value: 1000.

	"Phase 5: back to a single class; the site must still answer correctly."
	drive value: (Array with: IcTagC new) value: (Array with: 3) value: 2000.

	"Phase 6: force at least one scavenge (~80MB of garbage against the 64MB
	 young space) so every lookup cache is flushed while the now-megamorphic
	 site keeps its state, then drive all 7 classes again: the first sends
	 after the flush take the mega path with a COLD global cache, the exact
	 route where a broken link-register/return discipline in the shared probe
	 stub would hang or misdispatch."
	1 to: 10000 do: [:i | Array new: 1024].
	drive value: (OrderedCollection new
			add: IcTagA new; add: IcTagB new; add: IcTagC new; add: IcTagD new;
			add: IcTagE new; add: IcTagF new; add: IcTagG new; yourself)
		value: (OrderedCollection new
			add: 1; add: 2; add: 3; add: 4; add: 5; add: 6; add: 7; yourself)
		value: 500.

	fails = 0 ifTrue: [Transcript nextPutAll: 'IcPolymorphicSiteTest: OK'; lf].
	^fails
]
