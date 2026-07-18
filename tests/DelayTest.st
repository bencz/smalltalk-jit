"=== Standard test harness. Paste verbatim at the top of each tests/*.st file.
 Every check is wrapped so an unexpected runtime error counts as a FAILURE
 (a raw Assert failure or uncaught error prints a backtrace yet still exits 0,
 i.e. a false pass). The final top-level block MUST end with ^t report, whose
 integer return becomes the process exit code (0 = pass) that run_tests.sh
 gates on. Do not use the old Assert class in new tests. All catches use
 Exception (the root) so a wrong error-class name surfaces as a visible FAIL,
 never a silent exit-0. ==="
TestRun := Object [
    | fails count name |
    class named: aString [ ^self new setName: aString ]
    setName: aString [ name := aString. fails := 0. count := 0. ^self ]
    fail: aLabel [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' FAIL: '; nextPutAll: aLabel; lf.
    ]
    error: aLabel from: e [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' ERROR ['; nextPutAll: aLabel;
            nextPutAll: ']: '; nextPutAll: e messageText; lf.
    ]
    check: aLabel that: aBlock [
        count := count + 1.
        [ (aBlock value = true) ifFalse: [ self fail: aLabel ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    check: aLabel equals: expected is: aBlock [
        count := count + 1.
        [ | got |
          got := aBlock value.
          got = expected ifFalse: [
              self fail: aLabel, ' (got ', got printString, ' expected ', expected printString, ')' ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    expect: aLabel raises: aClass in: aBlock [
        count := count + 1.
        [ aBlock value. self fail: aLabel, ' (no error raised)' ]
            on: Exception do: [:e |
                (aClass notNil and: [ e isKindOf: aClass ]) ifFalse: [
                    self fail: aLabel, ' (raised ', e class printString, ' not ', aClass printString, ')' ] ].
    ]
    report [
        Transcript nextPutAll: name; nextPutAll: ': ';
            nextPutAll: (count - fails) printString; nextPutAll: '/';
            nextPutAll: count printString; nextPutAll: ' ok'; lf.
        ^fails
    ]
]

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
