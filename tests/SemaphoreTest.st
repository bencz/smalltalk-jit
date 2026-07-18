"Tests for Semaphore: counting wait/signal, blocking, and mutual exclusion,
 driven by the kernel TestRun. The scheduler runs one worker OS thread per
 core, so forked fibers execute in REAL parallelism: joins use Semaphores the
 workers signal (never counted yields), and a blocked state is given real time
 via a short Delay before being asserted (a waiter with no permit cannot
 proceed regardless, so the assert itself stays deterministic)."

[ SemRun := TestRun named: 'Semaphore' ]

"excess signals are counted, so that many waits pass without blocking"
[
	| s ran |
	s := Semaphore new.
	s signal; signal.
	ran := false.
	s wait. s wait.       "consumes the two permits, does not block"
	ran := true.
	SemRun check: 'two counted permits satisfied two waits' that: [ ran ].
]

"wait blocks until a signal arrives; ordering proves the block. The worker
 cannot append before the signal (no permit), and the main process only reads
 the log again after the worker's done signal, so the log is never mutated
 concurrently with a read."
[
	| s done log |
	s := Semaphore new.
	done := Semaphore new.
	log := OrderedCollection new.
	[ s wait. log addLast: #worker. done signal ] fork.
	log addLast: #beforeDelay.
	(Delay forMilliseconds: 20) wait.      "give the worker time to park in wait"
	log addLast: #afterDelay.
	SemRun check: 'worker still blocked before signal' equals: 2 is: [ log size ].
	s signal.
	done wait.
	SemRun check: 'worker ran after signal' equals: 3 is: [ log size ].
	SemRun check: 'worker event is last' equals: #worker is: [ log at: 3 ].
]

"forMutualExclusion starts with one permit"
[
	| mutex done entered |
	mutex := Semaphore forMutualExclusion.
	done := Semaphore new.
	entered := false.
	mutex wait.                 "acquire; does not block (count was 1)"
	[ mutex wait. entered := true. done signal ] fork.
	(Delay forMilliseconds: 20) wait.      "give the second waiter time to park"
	SemRun check: 'second waiter blocked while mutex held' that: [ entered = false ].
	mutex signal.               "release; wakes the waiter"
	done wait.
	SemRun check: 'second waiter ran after release' that: [ entered ].
]

"critical: gives mutual exclusion across parallel workers"
[
	| mutex shared done |
	mutex := Semaphore forMutualExclusion.
	shared := 0.
	done := Semaphore new.
	1 to: 5 do: [:w |
		[ 1 to: 20 do: [:i | mutex critical: [ shared := shared + 1 ] ]. done signal ] fork ].
	1 to: 5 do: [:w | done wait ].
	SemRun check: 'critical sections excluded each other' equals: 100 is: [ shared ].
]

"critical: releases the lock even when the block raises"
[
	| mutex |
	mutex := Semaphore forMutualExclusion.
	SemRun expect: 'error inside critical propagates' raises: Error
		in: [ mutex critical: [ Error signal: 'boom' ] ].
	"lock must be free again: a fresh critical section runs without blocking"
	SemRun check: 'lock is free after unwind' equals: 42 is: [ mutex critical: [ 42 ] ].
]

[ ^SemRun report ]
