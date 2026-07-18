"Stress test for the garbage collector under concurrency, targeting the JIT's
 inlined control flow (ifTrue:ifFalse:/and:/or:) inside forked fibers. Many
 fibers run allocation-heavy loops whose values flow through nested inlined
 conditionals, yielding mid-loop so a scavenge can run while the conditionals'
 result temporaries are live on a suspended fiber's stack. Each fiber's result
 is fully deterministic, so it is recomputed single-threaded and every fiber
 must match: GC corruption of a live slot would change a result and fail.
 CONCURRENCY DISCIPLINE: the scheduler runs one worker OS thread per core, so
 forked fibers execute in REAL parallelism. Cross-fiber results therefore go
 into DISJOINT slots of a preallocated Array (never a shared growing
 OrderedCollection), joins use Semaphores (never counted yields), and every
 channel has a single direction (a dedicated reply channel for the response;
 a request channel must never double as the reply channel, or the requester
 races the consumer for its own residual items)."

[
    | t n iters results done garbage expected ch reply rounds acc levels perLevel totals topDone |
    t := TestRun named: 'GcStress'.

    "--- 64 fibers computing the same deterministic accumulator through nested
     inlined conditionals while the GC churns; each fiber owns slot i ---"
    n := 64.
    iters := 400.
    results := Array new: n.
    done := Semaphore new.

    1 to: n do: [:i |
        [ | acc2 j |
          acc2 := 0.
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
            acc2 := acc2 + val.
            (j \\ 32 = 0) ifTrue: [ Processor yield ] ].     "suspend mid-flight"
          results at: i put: acc2.
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

    t check: 'every fiber slot was filled' that: [
        | ok | ok := true.
        1 to: n do: [:i | (results at: i) isNil ifTrue: [ ok := false ] ].
        ok ].
    t check: 'every fiber accumulator is exact' that: [
        | ok | ok := true.
        1 to: n do: [:i | (results at: i) = expected ifFalse: [ ok := false ] ].
        ok ].

    "--- mixed workload: a channel worker folds thousands of values through
     inlined conditionals while the producer allocates on every round; the
     response comes back on a DEDICATED reply channel ---"
    rounds := 6000.
    ch := Channel new.
    reply := Channel new.

    [ | total running |
      total := 0.
      running := true.
      [ running ] whileTrue: [
        | v |
        v := ch receive.
        v = #stop
            ifTrue: [ running := false. reply send: total ]
            ifFalse: [ (v even)
                ifTrue: [ total := total + v ]
                ifFalse: [ total := total - 1 ] ] ] ] fork.

    1 to: rounds do: [:i |
        | scratch |
        scratch := Array new: 4.                    "allocation pressure"
        ch send: i.
        (i \\ 100 = 0) ifTrue: [ Processor yield ] ].
    ch send: #stop.
    acc := reply receive.

    "even numbers 2..6000 add up; each of the 3000 odd numbers subtracts 1"
    t check: 'channel fold total is exact' equals: ((2 * (3000 * 3001 // 2)) - 3000) is: [ acc ].

    "--- deeply nested forks: fibers that fork more fibers, each innermost
     fiber owning a disjoint slot, joins funneled through semaphores ---"
    levels := 8.
    perLevel := 12.
    totals := Array new: levels * perLevel.
    topDone := Semaphore new.

    1 to: levels do: [:l |
        [ | sub |
          sub := Semaphore new.
          1 to: perLevel do: [:i |
            [ | s |
              s := 0.
              1 to: 50 do: [:k | | a | a := Array new: 2. s := s + k ].
              totals at: (l - 1) * perLevel + i put: s.
              sub signal ] fork ].
          1 to: perLevel do: [:i | sub wait ].
          topDone signal ] fork ].

    1 to: levels do: [:l | topDone wait ].

    t check: 'every nested fiber slot was filled' that: [
        | ok | ok := true.
        1 to: levels * perLevel do: [:i | (totals at: i) isNil ifTrue: [ ok := false ] ].
        ok ].
    "each innermost fiber sums 1..50 = 1275"
    t check: 'nested fork totals are exact' equals: (levels * perLevel * 1275) is: [
        | sum | sum := 0.
        1 to: levels * perLevel do: [:i | sum := sum + (totals at: i) ].
        sum ].

    ^t report
]
