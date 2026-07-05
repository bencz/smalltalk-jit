"Tests for Semaphore: counting wait/signal, blocking, and mutual exclusion."

"excess signals are counted, so that many waits pass without blocking"
[
	| s ran |
	s := Semaphore new.
	s signal; signal.
	ran := false.
	s wait. s wait.       "consumes the two permits, does not block"
	ran := true.
	Assert true: ran.
]

"wait blocks until a signal arrives; ordering proves the block"
[
	| s log |
	s := Semaphore new.
	log := OrderedCollection new.
	[ s wait. log add: #worker ] fork.
	log add: #beforeYield.
	Processor yield.            "worker runs, blocks on wait"
	log add: #afterYield.
	Assert true: log size = 2.  "worker still blocked, has not added #worker"
	s signal.
	Processor yield.            "worker wakes and finishes"
	Assert true: log size = 3.
	Assert true: (log at: 3) = #worker.
]

"forMutualExclusion starts with one permit"
[
	| mutex log |
	mutex := Semaphore forMutualExclusion.
	log := OrderedCollection new.
	mutex wait.                 "acquire; does not block (count was 1)"
	[ mutex wait. log add: #second ] fork.
	Processor yield.            "second waiter blocks"
	Assert true: log isEmpty.
	mutex signal.               "release; wakes the waiter"
	Processor yield.
	Assert true: log size = 1.
]

"critical: gives mutual exclusion and releases even on unwind"
[
	| mutex shared |
	mutex := Semaphore forMutualExclusion.
	shared := 0.
	1 to: 5 do: [:w |
		[ 1 to: 20 do: [:i | mutex critical: [ shared := shared + 1 ] ] ] fork ].
	1 to: 40 do: [:i | Processor yield].
	Assert true: shared = 100.
]

"critical: releases the lock even when the block raises"
[
	| mutex |
	mutex := Semaphore forMutualExclusion.
	Assert do: [ mutex critical: [ Error signal: 'boom' ] ] expect: Error.
	"lock must be free again: a fresh critical section runs without blocking"
	Assert true: (mutex critical: [ 42 ]) = 42.
]
