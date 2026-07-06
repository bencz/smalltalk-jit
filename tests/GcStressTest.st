"Stress test for the garbage collector under concurrency, targeting the JIT's
 inlined control flow (ifTrue:ifFalse:/and:/or:) inside forked fibers. Many
 fibers run allocation-heavy loops whose values flow through nested inlined
 conditionals, yielding mid-loop so a scavenge can run while the conditionals'
 result temporaries are live on a suspended fiber's stack. Each fiber's result
 is fully deterministic, so it is recomputed single-threaded and every fiber must
 match — GC corruption of a live slot would change a result and fail the test."

"64 fibers computing the same deterministic accumulator through nested inlined
 conditionals while the GC churns"
[
	| n iters results done garbage expected |
	n := 64.
	iters := 400.
	results := OrderedCollection new.
	done := Semaphore new.

	1 to: n do: [:i |
		[ | acc j |
		  acc := 0.
		  j := 0.
		  [ j < iters ] whileTrue: [
			| box val |
			j := j + 1.
			box := Array new: 5.                    "allocate every iteration"
			val := ((j \\ 3 = 0) and: [ j > 20 ])
				ifTrue: [ box at: 1 put: j. j * 2 ]
				ifFalse: [ ((j \\ 5 = 0) or: [ j < 10 ])
					ifTrue: [ box at: 2 put: j negated. j + 1 ]
					ifFalse: [ box at: 3 put: 0. 1 ] ].
			acc := acc + val.
			(j \\ 32 = 0) ifTrue: [ Processor yield ] ].     "suspend mid-flight"
		  results addLast: acc.
		  done signal ] fork ].

	"keep the scheduler + allocator busy so scavenges fire while fibers are parked"
	1 to: (n * 3) do: [:r |
		garbage := OrderedCollection new.
		1 to: 80 do: [:k | garbage addLast: (Array new: 3) ].
		Processor yield ].

	1 to: n do: [:i | done wait ].

	"recompute the per-fiber accumulator single-threaded (no fibers, no GC race)"
	expected := 0.
	1 to: iters do: [:j |
		| val |
		val := ((j \\ 3 = 0) and: [ j > 20 ])
			ifTrue: [ j * 2 ]
			ifFalse: [ ((j \\ 5 = 0) or: [ j < 10 ])
				ifTrue: [ j + 1 ]
				ifFalse: [ 1 ] ].
		expected := expected + val ].

	Assert true: results size = n.
	results do: [:a | Assert true: a = expected ].
]

"mixed workload: a channel worker folds thousands of values through inlined
 conditionals while the producer allocates on every round — the whole thing runs
 across many scavenges and the folded total must be exact"
[
	| rounds ch acc |
	rounds := 6000.
	ch := Channel new.

	[ | total running |
	  total := 0.
	  running := true.
	  [ running ] whileTrue: [
		| v |
		v := ch receive.
		v = #stop
			ifTrue: [ running := false. ch send: total ]
			ifFalse: [ (v even)
				ifTrue: [ total := total + v ]
				ifFalse: [ total := total - 1 ] ] ] ] fork.

	1 to: rounds do: [:i |
		| scratch |
		scratch := Array new: 4.                    "allocation pressure"
		ch send: i.
		(i \\ 100 = 0) ifTrue: [ Processor yield ] ].
	ch send: #stop.
	Processor yield.
	acc := ch receive.

	"even numbers 2..6000 add up; each of the 3000 odd numbers subtracts 1"
	Assert true: acc = ((2 * (3000 * 3001 // 2)) - 3000).
]

"deeply nested forks: fibers that fork more fibers, all funneling results back
 through a semaphore, to churn the scheduler registry and GC together"
[
	| levels perLevel total done |
	levels := 8.
	perLevel := 12.
	total := OrderedCollection new.
	done := Semaphore new.

	1 to: levels do: [:l |
		[ | subtotal sub |
		  subtotal := 0.
		  sub := Semaphore new.
		  1 to: perLevel do: [:i |
			[ | s |
			  s := 0.
			  1 to: 50 do: [:k | | a | a := Array new: 2. s := s + k ].
			  total addLast: s.
			  sub signal ] fork ].
		  1 to: perLevel do: [:i | sub wait ].
		  done signal ] fork ].

	1 to: levels do: [:l | done wait ].

	Assert true: total size = (levels * perLevel).
	"each innermost fiber sums 1..50 = 1275"
	Assert true: (total inject: 0 into: [:acc :s | acc + s]) = (levels * perLevel * 1275).
]
