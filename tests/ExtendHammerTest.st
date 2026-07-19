"Multi-worker stress for the extend/removeSelector STW invalidation: N real
 OS threads (Worker parallel:) hammer a hot send WHILE another worker
 repeatedly extends the receiver's class with an override and removes it
 again. Every send must answer the OLD or the NEW method result, never
 crash, tear, or dispatch stale code after the pause; the totals are checked
 against the exact iteration counts. This drives icInvalidateAllSends'
 stop-the-world bracket (park peers mid-JIT, swap dicts, reset every IC
 cell, epoch-flush the TLS lookup caches) from a worker-saturated heap.

 Worker blocks only return numbers (no asserts cross the thread boundary).

 WARNING: drives real OS threads; run ONLY inside the resource sandbox
 (systemd-run + taskset), never loose (project rule after the 2026-07-13
 desktop freeze)."

ExtHamBase := Object [
	probe [ ^1 ]
]

ExtHamTarget := ExtHamBase [
]

[ | t nWorkers perWorker rounds blocks results total |
	t := TestRun named: 'ExtendHammer'.
	nWorkers := 3.
	perWorker := 40000.
	rounds := 40.

	blocks := OrderedCollection new.
	1 to: nWorkers do: [ :w |
		blocks add: [ | sum r |
			sum := 0.
			1 to: perWorker do: [ :i |
				r := ExtHamTarget new probe.
				(r = 1 or: [ r = 2 ]) ifTrue: [ sum := sum + 1 ] ].
			sum ] ].
	"the flipper runs concurrently with the hammer workers"
	blocks add: [ | n |
		n := 0.
		1 to: rounds do: [ :round |
			Compiler new buildClass:
				(Parser parseString: 'ExtHamTarget extend [ probe [ ^2 ] ]') parseClass.
			ExtHamTarget removeSelector: #probe.
			n := n + 1 ].
		n ].

	results := Worker parallel: blocks asArray.

	total := 0.
	1 to: nWorkers do: [ :w | total := total + (results at: w) ].
	t check: 'every hammered send answered the old or the new method'
		equals: nWorkers * perWorker is: [ total ].
	t check: 'all extend/remove rounds completed'
		equals: rounds is: [ results at: nWorkers + 1 ].
	t check: 'the class ends in its base state (override removed)'
		equals: 1 is: [ ExtHamTarget new probe ].
	^t report
]
