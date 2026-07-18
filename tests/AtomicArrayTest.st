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

"=== AtomicArray: fixed-size array of atomic slots. Single-fiber correctness
 (no workers; contention lives in AtomicStressTest.st). This file goes BEYOND
 AtomicTest.st: larger array + per-slot independence, getAndIncrementAt:,
 at:put: answer, object-identity CAS/getAndSet, and full out-of-bounds and
 bad-argument error behaviour on every operation. ==="

[
    | t arr big o o2 sum n |
    t := TestRun named: 'AtomicArray'.

    "---- construction ---------------------------------------------------------"
    arr := AtomicArray new: 8.
    t check: 'new: size' equals: 8 is: [ arr size ].
    t check: 'all slots start nil' that: [
        | ok | ok := true.
        1 to: 8 do: [:i | (arr at: i) isNil ifFalse: [ ok := false ] ].
        ok ].

    "---- at:put: answers the stored value -------------------------------------"
    t check: 'at:put: answers value' equals: 5 is: [ arr at: 1 put: 5 ].
    t check: 'at: reads it back' equals: 5 is: [ arr at: 1 ].

    "---- getAndIncrementAt: (returns OLD; complements AtomicTest's incrementAndGetAt:) --"
    arr at: 2 put: 10.
    t check: 'getAndIncrementAt: returns old' equals: 10 is: [ arr getAndIncrementAt: 2 ].
    t check: 'getAndIncrementAt: applied' equals: 11 is: [ arr at: 2 ].
    t check: 'incrementAndGetAt: returns new' equals: 12 is: [ arr incrementAndGetAt: 2 ].

    "---- object-identity CAS and getAndSet in a slot --------------------------"
    o := Object new. o2 := Object new.
    arr at: 3 put: o.
    t check: 'slot holds object ref' that: [ (arr at: 3) == o ].
    t check: 'CAS object hit true' that: [ (arr at: 3 compareAndSet: o with: o2) = true ].
    t check: 'CAS object stored new' that: [ (arr at: 3) == o2 ].
    t check: 'CAS object miss false' that: [ (arr at: 3 compareAndSet: o with: o) = false ].
    t check: 'CAS object miss left value' that: [ (arr at: 3) == o2 ].
    t check: 'getAndSet object returns old' that: [ (arr at: 3 getAndSet: o) == o2 ].
    t check: 'getAndSet object stored new' that: [ (arr at: 3) == o ].

    "---- larger array: per-slot independence ----------------------------------"
    big := AtomicArray new: 50.
    1 to: 50 do: [:i | big at: i put: i ].
    t check: 'big: every slot holds its own value' that: [
        | ok | ok := true.
        1 to: 50 do: [:i | (big at: i) = i ifFalse: [ ok := false ] ].
        ok ].
    "getAndAdd across every slot: old is i, new is 2*i"
    t check: 'big: getAndAdd returns each old' that: [
        | ok | ok := true.
        1 to: 50 do: [:i | (big at: i getAndAdd: i) = i ifFalse: [ ok := false ] ].
        ok ].
    sum := 0.
    1 to: 50 do: [:i | sum := sum + (big at: i) ].
    t check: 'big: doubled-slot sum' equals: 2550 is: [ sum ].   "2 * (50*51/2)"
    "writing one slot leaves neighbours untouched"
    big at: 25 put: 777.
    t check: 'neighbour 24 untouched' equals: 48 is: [ big at: 24 ].
    t check: 'neighbour 26 untouched' equals: 52 is: [ big at: 26 ].

    "---- out-of-bounds: every op raises Error (indices are 1-based) ------------"
    t expect: 'at: 0 raises' raises: Error in: [ arr at: 0 ].
    t expect: 'at: size+1 raises' raises: Error in: [ arr at: 9 ].
    t expect: 'at:put: OOB raises' raises: Error in: [ arr at: 0 put: 1 ].
    t expect: 'compareAndSet: OOB raises' raises: Error in: [ arr at: 99 compareAndSet: 1 with: 2 ].
    t expect: 'getAndSet: OOB raises' raises: Error in: [ arr at: 99 getAndSet: 3 ].
    t expect: 'getAndAdd: OOB raises' raises: Error in: [ arr at: 99 getAndAdd: 1 ].

    "---- bad argument: getAndAdd: needs a SmallInteger ------------------------"
    arr at: 4 put: 100.
    t expect: 'getAndAdd: non-integer raises' raises: Error in: [ arr at: 4 getAndAdd: 2.5 ].
    t check: 'getAndAdd: failure left slot intact' equals: 100 is: [ arr at: 4 ].

    "=== AtomicInteger gaps not covered by AtomicTest.st (inherited Atomic API) ==="
    "value / value: aliases for get / set, getAndSet: on an AtomicInteger, on: with a
     negative seed. AtomicTest exercises the arithmetic; these are the untested seams."
    n := AtomicInteger on: -5.
    t check: 'AtomicInteger on: negative' equals: -5 is: [ n get ].
    t check: 'value alias reads get' equals: -5 is: [ n value ].
    n value: 8.
    t check: 'value: alias writes set' equals: 8 is: [ n get ].
    t check: 'inherited getAndSet: returns old' equals: 8 is: [ n getAndSet: 20 ].
    t check: 'inherited getAndSet: stored new' equals: 20 is: [ n get ].
    t check: 'addAndGet: 0 is a no-op' equals: 20 is: [ n addAndGet: 0 ].

    ^t report
]
