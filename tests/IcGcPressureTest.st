"Inline caches under GC pressure. The IC keys on the receiver's CLASS ADDRESS,
 and classes defined by this file are YOUNG: every scavenge moves them and
 recycles the abandoned semispace, so a cell surviving a collection unreset
 could false-hit on a recycled address (wrong method) or, with a moved class,
 miss forever (never rebinding to the class's new address). The loop binds the
 site, churns enough allocation to force many scavenges, and keeps asserting
 the answer, so either failure mode counts. Promotion eventually makes the
 class OLD, which then exercises the full-GC reset path too."

IcGcVictim := Object [ ping [ ^42 ] ]
IcGcOther := Object [ ping [ ^7 ] ]

IcGcSite := Object [
	class hit: x [ ^x ping ]
]

[ | fails victim other |
	fails := 0.
	victim := IcGcVictim new.
	other := IcGcOther new.

	1 to: 150 do: [:round |
		"send through the site: first sends of a round rebind after any reset"
		1 to: 200 do: [:i |
			(IcGcSite hit: victim) = 42 ifFalse: [fails := fails + 1]].
		"a second class at the same site keeps the fallback path honest too"
		(IcGcSite hit: other) = 7 ifFalse: [fails := fails + 1].
		"churn: ~1MB of garbage per round forces scavenges that move the young
		 classes and recycle their old addresses"
		1 to: 2000 do: [:i | Array new: 64].
		"a fresh receiver every few rounds also moves the instance itself"
		round \\ 10 = 0 ifTrue: [victim := IcGcVictim new]].

	fails = 0 ifTrue: [Transcript nextPutAll: 'IcGcPressureTest: OK'; lf].
	^fails
]
