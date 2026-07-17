"Tier-1 adaptive recompilation correctness (jit/Tier.h): methods crossing the
 invocation threshold (default 1000) in the MIDDLE of a hot loop are
 recompiled with their monomorphic sends promoted to guarded direct calls.
 Every answer is asserted per iteration, so any divergence between the tier-0
 and the tier-1 code, at the crossing or after it, is a counted failure. The
 churn phase then forces scavenges (cells reset, lookup caches flushed) so the
 same sites re-miss and rebind to the RECOMPILED code, and the identical
 workload is asserted again: phase 1 proves the crossing is seamless, phase 2
 proves the tier-1 code itself computes the same answers."

TierCoreA := Object [
	bump: x [ ^x + 3 ]
	double: x [ ^(self bump: x) + (self bump: x) ]
	weave: x [ ^(self double: x) - (self bump: x) ]
]

[ | fails a drive |
	fails := 0.
	a := TierCoreA new.
	"(a double: i) = 2i+6, (a weave: i) = (2i+6)-(i+3) = i+3, per iteration."
	drive := [:count |
		1 to: count do: [:i |
			(a double: i) = (i * 2 + 6) ifFalse: [fails := fails + 1].
			(a weave: i) = (i + 3) ifFalse: [fails := fails + 1]]].

	"Phase 1: 3000 invocations of each probe method, crossing the threshold
	 mid-loop. The loop's own sites keep dispatching the superseded code after
	 the recompile (valid forever); answers must not change."
	drive value: 3000.

	"Phase 2: ~80MB of garbage against the 64MB young space forces scavenges:
	 every IC cell resets and the lookup caches flush, so the SAME sites
	 re-miss and rebind to the tier-1 code. Same workload, same answers."
	1 to: 10000 do: [:i | Array new: 1024].
	drive value: 3000.

	fails = 0 ifTrue: [Transcript nextPutAll: 'TierCorrectnessTest: OK'; lf].
	^fails
]
