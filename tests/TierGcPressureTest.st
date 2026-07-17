"Tier-1 guards under GC pressure (jit/Tier.h): the promoted guard bakes the
 receiver CLASS as a code immediate. The classes here are young test-defined
 objects, so scavenges MOVE them and the stop-the-world pointer patch
 (pointersOffsets) must keep every baked guard pointing at the moved class.
 One shared site is heated monomorphic with class A and then driven with A
 and B alternating between churn rounds: a correctly patched guard keeps
 answering A's tag through the direct path, and B must NEVER match it (a
 false hit would answer 1 instead of 2, the counted discriminator)."

TierGcA := Object [ tag [ ^1 ] ]
TierGcB := Object [ tag [ ^2 ] ]

TierGcSite := Object [
	through: o [ ^o tag ]
]

[ | fails site a b |
	fails := 0.
	site := TierGcSite new.
	a := TierGcA new.
	b := TierGcB new.

	"Heat through: past the threshold with ONLY class A: its tag site is
	 promoted to an exact-TierGcA guard with a baked class pointer."
	1 to: 2500 do: [:i |
		(site through: a) = 1 ifFalse: [fails := fails + 1]].

	"Churn rounds move the young class objects (STW patches the baked guard),
	 then both classes drive the SAME site: A must keep hitting the patched
	 guard, B must keep missing it into the IC fallback."
	1 to: 6 do: [:round |
		1 to: 3000 do: [:i | Array new: 1024].
		1 to: 500 do: [:i |
			(site through: a) = 1 ifFalse: [fails := fails + 1].
			(site through: b) = 2 ifFalse: [fails := fails + 1]]].

	fails = 0 ifTrue: [Transcript nextPutAll: 'TierGcPressureTest: OK'; lf].
	^fails
]
