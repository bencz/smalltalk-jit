"Multi-worker inline-cache hammer: the direct regression test for the
 historical torn-read bug (a multi-word cache entry rewritten in place under
 lock-free readers dispatched the WRONG method; see core/Lookup.h). Here N OS
 threads (Worker parallel:, shared heap) pound the SAME send site (inside
 IcHamSite class>>probe:, one compiled method, one cell) alternating two
 receiver classes, WITH allocation churn in the loop so scavenges keep
 resetting the cell while peers race to rebind it. Any torn publish, use-after
 -free of a cache state, or wrong-method dispatch shows up as a wrong answer,
 counted per worker and asserted by the main fiber (worker blocks only return
 counts: no asserts cross the thread boundary).

 The worker closures are built from ONE block literal (collect:), so they
 share a single CompiledBlock/NativeCode: the sites inside the block are also
 one cell each, not per-worker copies.

 WARNING: drives real OS threads; the runner executes this file only inside
 the resource sandbox (see run_tests.sh), never loose."

IcHamA := Object [ who [ ^1 ] ]
IcHamB := Object [ who [ ^2 ] ]

IcHamSite := Object [
	class probe: x [ ^x who ]
]

[ | workers results fails perWorker |
	perWorker := 30000.
	workers := (1 to: 4) collect: [:w |
		[ | a b good |
			a := IcHamA new.
			b := IcHamB new.
			good := 0.
			1 to: perWorker do: [:i |
				(IcHamSite probe: a) = 1 ifTrue: [good := good + 1].
				(IcHamSite probe: b) = 2 ifTrue: [good := good + 1].
				"churn: keep the collector resetting cells under the race"
				i \\ 16 = 0 ifTrue: [Array new: 32]].
			good ]].

	results := Worker parallel: workers.

	fails := 0.
	results do: [:r |
		r = (perWorker * 2) ifFalse: [fails := fails + 1]].

	fails = 0 ifTrue: [Transcript nextPutAll: 'IcHammerTest: OK'; lf].
	^fails
]
