"Channel semantics under the MULTI-WORKER scheduler (one worker OS thread per
 core, fibers pinned round-robin: real parallelism). Pinned regression for the
 request/reply topology bug once hidden in GcStressTest: a single channel used
 for both directions lets the requester steal its own queued items as the
 'reply'. Every scenario here uses one channel per direction and exact totals,
 so a lost, duplicated, or misrouted item fails the count.
 Scenario 1 (SPSC, no yields) maximizes the consumer backlog and exercises the
 queue's growth path under the channel monitor stripe. Scenario 2 (MPMC 8x8)
 hammers concurrent send/receive from many workers with disjoint ranges.
 Scenario 3 (ping-pong RPC) alternates empty-queue handoffs in both directions,
 stressing the park/resume (parkPending) path of the semaphore protocol."

[
    | t rounds jobs reply total expected producers consumers perProducer results prodDone sum pings pongs turns |
    t := TestRun named: 'ChannelMultiWorker'.

    "--- 1: SPSC with reply channel, no yields: maximum backlog ---"
    rounds := 20000.
    jobs := Channel new.
    reply := Channel new.
    [ | acc running |
      acc := 0.
      running := true.
      [ running ] whileTrue: [
        | v |
        v := jobs receive.
        v = #stop
            ifTrue: [ running := false. reply send: acc ]
            ifFalse: [ acc := acc + v ] ] ] fork.
    1 to: rounds do: [:i |
        | scratch |
        scratch := Array new: 3.   "allocation pressure per round"
        jobs send: i ].
    jobs send: #stop.
    total := reply receive.
    expected := rounds * (rounds + 1) // 2.
    t check: 'SPSC backlog fold is exact' equals: expected is: [ total ].

    "--- 2: MPMC 8 producers x 8 consumers, disjoint ranges, exact grand total ---"
    producers := 8.
    consumers := 8.
    perProducer := 2000.
    jobs := Channel new.
    results := Channel new.
    1 to: consumers do: [:c |
        [ | acc running |
          acc := 0.
          running := true.
          [ running ] whileTrue: [
            | v |
            v := jobs receive.
            v = #stop
                ifTrue: [ running := false. results send: acc ]
                ifFalse: [ acc := acc + v ] ] ] fork ].
    "the stops must be queued AFTER every payload send, so join the producers
     through a semaphore each signals when its range is fully pushed"
    prodDone := Semaphore new.
    1 to: producers do: [:p |
        [ | base |
          base := (p - 1) * perProducer.
          1 to: perProducer do: [:i | jobs send: base + i ].
          prodDone signal ] fork ].
    1 to: producers do: [:p | prodDone wait ].
    1 to: consumers do: [:c | jobs send: #stop ].
    sum := 0.
    1 to: consumers do: [:c | sum := sum + results receive ].
    expected := producers * perProducer * (producers * perProducer + 1) // 2.
    t check: 'MPMC grand total is exact' equals: expected is: [ sum ].

    "--- 3: ping-pong RPC, two channels, empty-queue handoffs both ways ---"
    pings := Channel new.
    pongs := Channel new.
    turns := 5000.
    [ | running |
      running := true.
      [ running ] whileTrue: [
        | v |
        v := pings receive.
        v = #stop
            ifTrue: [ running := false ]
            ifFalse: [ pongs send: v + 1 ] ] ] fork.
    total := 0.
    1 to: turns do: [:i |
        pings send: i.
        total := total + (pongs receive) ].
    pings send: #stop.
    expected := (turns * (turns + 1) // 2) + turns.
    t check: 'ping-pong round trips are exact' equals: expected is: [ total ].

    ^t report
]
