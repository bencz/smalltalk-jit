"Stress test for Channel under heavy concurrent load. Thousands of blocking
 send/receive hand-offs (each a fiber suspend/resume) plus steady allocation
 force many scavenges while fibers are parked. Every scenario checks an exact
 sum/count on the main process, so any lost, duplicated, or GC-corrupted item
 fails the test."

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
	Assert true: total = expected.
]

"pipeline: producer -> even-filter -> collector over chained channels; the exact
 set of even numbers must survive the two hand-offs"
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

	Assert true: collected size = 2500.
	total := 0.
	collected do: [:n | total := total + n ].
	Assert true: total = (2 * (2500 * 2501 // 2)).
]

"many independent channels, each handed off between a dedicated producer and
 consumer fiber, all interleaved under GC pressure from a churner fiber"
[
	| n channels sums garbage grandTotal |
	n := 60.
	channels := OrderedCollection new.
	sums := OrderedCollection new.

	1 to: n do: [:i |
		| ch |
		ch := Channel new.
		channels addLast: ch.
		[ | s |
		  s := 0.
		  1 to: 150 do: [:k | s := s + ch receive ].
		  sums addLast: s ] fork ].

	[ 1 to: 300 do: [:r |
		garbage := OrderedCollection new.
		1 to: 30 do: [:k | garbage addLast: (Array new: 4) ].
		Processor yield ] ] fork.

	1 to: n do: [:i |
		| ch |
		ch := channels at: i.
		[ 1 to: 150 do: [:k | ch send: k ] ] fork ].

	1 to: 80 do: [:i | Processor yield ].

	Assert true: sums size = n.
	grandTotal := 0.
	sums do: [:s | grandTotal := grandTotal + s ].
	"each consumer sums 1..150 = 11325"
	Assert true: grandTotal = (n * 11325).
]
