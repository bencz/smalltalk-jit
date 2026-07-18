"=== Delay: a duration that parks the current process on wait. Delay has no
 public duration accessor, so we cover construction + identity for all three
 factory selectors, and exercise wait DETERMINISTICALLY and SINGLE-FIBER:
 zero-duration waits (return immediately) plus EXACTLY ONE short (5 ms) elapsing
 wait, checking only that it returns and that wall-clock time did not go
 backwards. No loop over delays, no long sleeps, no workers. ==="

[
    | t d0 same t0 t1 |
    t := TestRun named: 'Delay'.

    "---- construction: every factory answers a Delay --------------------------"
    t check: 'forMicroseconds: answers a Delay' that: [ (Delay forMicroseconds: 1000) isKindOf: Delay ].
    t check: 'forMilliseconds: answers a Delay' that: [ (Delay forMilliseconds: 5) isKindOf: Delay ].
    t check: 'forSeconds: answers a Delay (not waited)' that: [ (Delay forSeconds: 30) isKindOf: Delay ].
    t check: 'forMicroseconds: 0 answers a Delay' that: [ (Delay forMicroseconds: 0) isKindOf: Delay ].

    "distinct constructions are distinct objects"
    t check: 'two Delays are distinct instances' that: [
        ((Delay forMilliseconds: 1) == (Delay forMilliseconds: 1)) = false ].

    "---- wait answers the receiver (^self) ------------------------------------"
    d0 := Delay forMicroseconds: 0.
    t check: 'zero-microsecond wait answers a Delay' that: [ d0 wait isKindOf: Delay ].
    same := Delay forMilliseconds: 0.
    t check: 'wait answers the same receiver (identity)' that: [ (same wait) == same ].

    "---- clock sanity ----------------------------------------------------------"
    t check: 'currentMicroTime is an Integer' that: [ DateTime currentMicroTime isKindOf: Integer ].
    t check: 'currentMicroTime is positive' that: [ DateTime currentMicroTime > 0 ].

    "---- EXACTLY ONE short elapsing wait: returns, time does not go backwards --"
    t0 := DateTime currentMicroTime.
    (Delay forMilliseconds: 5) wait.
    t1 := DateTime currentMicroTime.
    t check: 'after a 5 ms wait, time did not go backwards' that: [ t1 >= t0 ].
    t check: 'elapsed window is bounded (< 5 s)' that: [ (t1 - t0) < 5000000 ].

    ^t report
]
