"Stress test for Channel under heavy concurrent load, driven by the kernel
 TestRun. Thousands of blocking send/receive hand-offs (each a fiber
 suspend/resume) plus steady allocation force many scavenges while fibers are
 parked. Every scenario checks an exact sum/count on the main process, so any
 lost, duplicated, or GC-corrupted item fails the test. Under the multi-worker
 pool fibers run in real parallelism: cross-fiber results go into DISJOINT
 slots of a preallocated Array (never a shared growing collection), joins use
 Semaphores the fibers signal (never counted yields), and helper fibers are
 joined before their block ends."

[ ChanStressRun := TestRun named: 'ChannelStress' ]

"worker pool: sum of 1..N computed across many workers over one channel pair"
[
	| workers jobs jobsCh resultsCh total done expected |
	workers := 16.
	jobs := 8000.
	jobsCh := Channel new.
	resultsCh := Channel new.

	1 to: workers do: [:w |
		[ | running |
		  running := true.
		  [ running ] whileTrue: [
			| job |
			job := jobsCh receive.
			job = #stop
				ifTrue: [ running := false. resultsCh send: #done ]
				ifFalse: [ resultsCh send: job ] ] ] fork ].

	1 to: jobs do: [:n | jobsCh send: n ].
	1 to: workers do: [:w | jobsCh send: #stop ].

	total := 0.
	done := 0.
	[ done < workers ] whileTrue: [
		| r |
		r := resultsCh receive.
		r = #done
			ifTrue: [ done := done + 1 ]
			ifFalse: [ total := total + r ] ].

	expected := jobs * (jobs + 1) // 2.
	ChanStressRun check: 'worker pool fold is exact' equals: expected is: [ total ].
]

"pipeline: producer -> even-filter -> collector over chained channels; the exact
 set of even numbers must survive the two hand-offs (the collector runs on the
 main process, so `collected` has a single mutator)"
[
	| source evens collected running total |
	source := Channel new.
	evens := Channel new.

	[ | go |
	  go := true.
	  [ go ] whileTrue: [
		| n |
		n := source receive.
		n = #stop
			ifTrue: [ go := false. evens send: #stop ]
			ifFalse: [ n even ifTrue: [ evens send: n ] ] ] ] fork.

	collected := OrderedCollection new.
	[ 1 to: 5000 do: [:i | source send: i ]. source send: #stop ] fork.

	running := true.
	[ running ] whileTrue: [
		| n |
		n := evens receive.
		n = #stop
			ifTrue: [ running := false ]
			ifFalse: [ collected addLast: n ] ].

	ChanStressRun check: 'pipeline kept every even number' equals: 2500 is: [ collected size ].
	total := 0.
	collected do: [:n | total := total + n ].
	ChanStressRun check: 'pipeline sum is exact' equals: 2 * (2500 * 2501 // 2) is: [ total ].
]

"many independent channels, each handed off between a dedicated producer and
 consumer fiber, all interleaved under GC pressure from a churner fiber; each
 consumer owns a disjoint sums slot and signals a join semaphore"
[
	| n channels sums garbage grandTotal consDone churnDone |
	n := 60.
	channels := OrderedCollection new.
	sums := Array new: n.
	consDone := Semaphore new.
	churnDone := Semaphore new.

	1 to: n do: [:i |
		| ch |
		ch := Channel new.
		channels addLast: ch.
		[ | s |
		  s := 0.
		  1 to: 150 do: [:k | s := s + ch receive ].
		  sums at: i put: s.
		  consDone signal ] fork ].

	[ 1 to: 300 do: [:r |
		garbage := OrderedCollection new.
		1 to: 30 do: [:k | garbage addLast: (Array new: 4) ].
		Processor yield ].
	  churnDone signal ] fork.

	1 to: n do: [:i |
		| ch |
		ch := channels at: i.
		[ 1 to: 150 do: [:k | ch send: k ] ] fork ].

	1 to: n do: [:i | consDone wait ].
	churnDone wait.

	ChanStressRun check: 'every consumer slot was filled' that: [
		| ok | ok := true.
		1 to: n do: [:i | (sums at: i) isNil ifTrue: [ ok := false ] ].
		ok ].
	grandTotal := 0.
	1 to: n do: [:i | grandTotal := grandTotal + (sums at: i) ].
	"each consumer sums 1..150 = 11325"
	ChanStressRun check: 'grand total across channels is exact' equals: n * 11325 is: [ grandTotal ].
]

[ ^ChanStressRun report ]
