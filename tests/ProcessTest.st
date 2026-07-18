"Tests for green processes (fibers): fork, yield, terminate, identity, driven
 by the kernel TestRun. The scheduler runs one worker OS thread per core, so a
 forked fiber starts in REAL parallelism: nothing may assume a block has or
 has not run after a yield. Not-yet-run states are made observable by gating
 the forked body on a Semaphore, joins use Semaphores or bounded polls, and
 each fiber writes only its own log. All assertions run on the main process
 (an assertion failing inside a forked process would be isolated and would
 not fail the test)."

[ ProcessRun := TestRun named: 'Process' ]

"fork answers a Process and actually runs the block; the body is gated on a
 semaphore so the not-yet-run state is deterministic, not a scheduling race"
[
	| ran gate done p |
	ran := 0.
	gate := Semaphore new.
	done := Semaphore new.
	p := [ gate wait. ran := ran + 1. done signal ] fork.
	ProcessRun check: 'fork answers a Process' that: [ p isKindOf: Process ].
	ProcessRun check: 'gated forked body has not run' equals: 0 is: [ ran ].
	gate signal.
	done wait.
	ProcessRun check: 'forked body ran exactly once' equals: 1 is: [ ran ].
]

"two forked fibers each make full ordered progress. The cooperative strict
 a/b alternation is undefined under parallel workers; what remains defined is
 that each fiber completes every step in program order, so each fiber records
 into its OWN log and the main process reads only after both joins."
[
	| logA logB done |
	logA := OrderedCollection new.
	logB := OrderedCollection new.
	done := Semaphore new.
	[ 1 to: 3 do: [:i | logA addLast: i. Processor yield ]. done signal ] fork.
	[ 1 to: 3 do: [:i | logB addLast: i. Processor yield ]. done signal ] fork.
	done wait. done wait.
	ProcessRun check: 'fiber A completed all steps' equals: 3 is: [ logA size ].
	ProcessRun check: 'fiber A steps are in program order' that: [
		(logA at: 1) = 1 and: [ (logA at: 2) = 2 and: [ (logA at: 3) = 3 ] ] ].
	ProcessRun check: 'fiber B completed all steps' equals: 3 is: [ logB size ].
	ProcessRun check: 'fiber B steps are in program order' that: [
		(logB at: 1) = 1 and: [ (logB at: 2) = 2 and: [ (logB at: 3) = 3 ] ] ].
]

"a process id is stable within a process and unique across processes"
[
	| mine other done |
	mine := Processor currentId.
	ProcessRun check: 'currentId is stable within a process' equals: mine is: [ Processor currentId ].
	other := mine.
	done := Semaphore new.
	[ other := Processor currentId. done signal ] fork.
	done wait.
	ProcessRun check: 'forked process has a distinct id' that: [ other ~= mine ].
]

"terminating a process before it enters its body prevents it from executing;
 the body is gated so it cannot have started, and the permit arriving after
 the terminate must not resurrect it. The victim must be STABLY PARKED when
 terminated: the scheduler defines terminate for READY and SUSPENDED fibers,
 while a fiber caught RUNNING or mid-park on another worker is deliberately
 left alone (cross-worker terminate is a documented later increment), so the
 short delay lets the fiber commit to SUSPENDED on the gate first."
[
	| ran gate p |
	ran := false.
	gate := Semaphore new.
	p := [ gate wait. ran := true ] fork.
	(Delay forMilliseconds: 50) wait.      "let p park on the gate (SUSPENDED)"
	p terminate.
	gate signal.                "a permit becomes available, but p is dead"
	(Delay forMilliseconds: 50) wait.
	ProcessRun check: 'terminated process never ran its body' that: [ ran = false ].
]

"a process can terminate itself; code after terminate does not run"
[
	| before reached tries |
	before := false.
	reached := false.
	[ before := true. Processor thisProcess terminate. reached := true ] fork.
	tries := 0.
	[ before or: [ tries > 400 ] ] whileFalse: [
		tries := tries + 1. (Delay forMilliseconds: 5) wait ].
	ProcessRun check: 'self-terminating process ran up to terminate' that: [ before ].
	(Delay forMilliseconds: 50) wait.
	ProcessRun check: 'code after self terminate never ran' that: [ reached = false ].
]

[ ^ProcessRun report ]
