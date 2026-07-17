"Multi-worker tier hammer (jit/Tier.h): N OS threads (Worker parallel:,
 shared heap) drive the SAME methods across their invocation thresholds at
 the same time, with allocation churn keeping scavenges resetting IC cells
 while the once-only recompile races its own trigger echoes. Torn publishes
 of the fresh NativeCode, a double recompile, or a promoted guard reading a
 freed IC state would show up as a wrong answer or a crash; wrong answers are
 counted per worker and asserted by the main fiber (worker blocks only return
 counts: no asserts cross the thread boundary).

 The worker closures are built from ONE block literal (collect:), so all
 workers share a single CompiledBlock/NativeCode and pound the same cells.

 WARNING: drives real OS threads; the runner executes this file only inside
 the resource sandbox (see run_tests.sh), never loose."

TierHamA := Object [ who [ ^1 ] ]
TierHamB := Object [ who [ ^2 ] ]

TierHamSite := Object [
	class probe: x [ ^x who ]
	class monoProbe: x [ ^(x who) + 10 ]
]

[ | workers results fails perWorker |
	perWorker := 20000.
	workers := (1 to: 4) collect: [:w |
		[ | a b good |
			a := TierHamA new.
			b := TierHamB new.
			good := 0.
			1 to: perWorker do: [:i |
				"probe:'s site sees both classes (promotes way 0, fails the
				 guard for the other class); monoProbe:'s site stays mono A."
				(TierHamSite probe: a) = 1 ifTrue: [good := good + 1].
				(TierHamSite probe: b) = 2 ifTrue: [good := good + 1].
				(TierHamSite monoProbe: a) = 11 ifTrue: [good := good + 1].
				"churn: keep the collector resetting cells under the race"
				i \\ 16 = 0 ifTrue: [Array new: 32]].
			good ]].

	results := Worker parallel: workers.

	fails := 0.
	results do: [:r |
		r = (perWorker * 3) ifFalse: [fails := fails + 1]].

	fails = 0 ifTrue: [Transcript nextPutAll: 'TierHammerTest: OK'; lf].
	^fails
]
