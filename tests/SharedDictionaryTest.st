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

"=== SharedDictionary: monitor-serialized thread-safe map. Single-fiber
 correctness (no workers). Covers the full protocol plus the two features that
 distinguish it from a plain / concurrent Dictionary: withAll: construction and
 a stored nil told apart from an absent key by the private sentinel. ==="

[
    | t d calls r base sum cnt ksum assocSum nilD missing empty |
    t := TestRun named: 'SharedDictionary'.

    "---- construction + empty state -------------------------------------------"
    d := SharedDictionary new.
    t check: 'new isEmpty' that: [ d isEmpty ].
    t check: 'new notEmpty false' that: [ d notEmpty = false ].
    t check: 'new size 0' equals: 0 is: [ d size ].

    "---- at:put: / at: / includesKey: -----------------------------------------"
    t check: 'at:put: answers value' equals: 42 is: [ d at: #a put: 42 ].
    t check: 'at: retrieves' equals: 42 is: [ d at: #a ].
    t check: 'includesKey present' that: [ d includesKey: #a ].
    t check: 'includesKey absent false' that: [ (d includesKey: #zzz) = false ].
    t check: 'size 1' equals: 1 is: [ d size ].
    t check: 'notEmpty after put' that: [ d notEmpty ].
    d at: #a put: 43.
    t check: 'overwrite updates value' equals: 43 is: [ d at: #a ].
    t check: 'overwrite keeps size 1' equals: 1 is: [ d size ].

    "---- at:ifAbsent: / at:ifPresent: -----------------------------------------"
    t check: 'at:ifAbsent: present returns value' equals: 43 is: [ d at: #a ifAbsent: [ #dflt ] ].
    t check: 'at:ifAbsent: absent runs block' equals: #dflt is: [ d at: #missing ifAbsent: [ #dflt ] ].
    t check: 'at:ifPresent: present maps value' equals: 86 is: [ d at: #a ifPresent: [:v | v * 2 ] ].
    t check: 'at:ifPresent: absent answers nil' that: [ (d at: #nope ifPresent: [:v | v ]) isNil ].

    "---- at:ifAbsentPut: -------------------------------------------------------"
    calls := 0.
    r := d at: #a ifAbsentPut: [ calls := calls + 1. 999 ].
    t check: 'ifAbsentPut present returns existing' equals: 43 is: [ r ].
    t check: 'ifAbsentPut present skips block' equals: 0 is: [ calls ].
    r := d at: #new ifAbsentPut: [ calls := calls + 1. 77 ].
    t check: 'ifAbsentPut absent returns computed' equals: 77 is: [ r ].
    t check: 'ifAbsentPut absent stored it' equals: 77 is: [ d at: #new ].
    t check: 'ifAbsentPut absent ran block once' equals: 1 is: [ calls ].

    "---- a stored nil is NOT absent (private sentinel) ------------------------"
    nilD := SharedDictionary new.
    nilD at: #k put: nil.
    t check: 'includesKey after storing nil' that: [ nilD includesKey: #k ].
    t check: 'size counts the nil entry' equals: 1 is: [ nilD size ].
    t check: 'at:ifAbsent: returns the stored nil not the block' that: [
        (nilD at: #k ifAbsent: [ #wouldBeAbsent ]) isNil ].
    t check: 'at:ifPresent: runs block on the stored nil' that: [
        nilD at: #k ifPresent: [:v | v isNil ] ].
    t check: 'notEmpty with only a nil value' that: [ nilD notEmpty ].

    "---- missing-key errors (receiver bound to a temp: safe) ------------------"
    missing := SharedDictionary new.
    t expect: 'at: on missing raises NotFoundError' raises: NotFoundError in: [ missing at: #ghost ].
    t expect: 'removeKey: on missing raises NotFoundError' raises: NotFoundError in: [ missing removeKey: #ghost ].
    t check: 'removeKey:ifAbsent: absent runs block' equals: #gone is: [ missing removeKey: #x ifAbsent: [ #gone ] ].

    "---- withAll: copies an existing Dictionary -------------------------------"
    base := Dictionary new.
    base at: #one put: 1.
    base at: #two put: 2.
    base at: #three put: 3.
    d := SharedDictionary withAll: base.
    t check: 'withAll: size' equals: 3 is: [ d size ].
    t check: 'withAll: copied value' equals: 2 is: [ d at: #two ].
    t check: 'withAll: is a distinct object' that: [ (d == base) = false ].
    "mutating the source must not touch the copy"
    base at: #four put: 4.
    t check: 'withAll: source add did not leak' that: [ (d includesKey: #four) = false ].

    "---- bulk insert ----------------------------------------------------------"
    d := SharedDictionary new.
    1 to: 200 do: [:i | d at: i put: i * 10 ].
    t check: 'bulk size 200' equals: 200 is: [ d size ].
    t check: 'all 200 keys retrievable' that: [
        | ok | ok := true.
        1 to: 200 do: [:i | (d at: i) = (i * 10) ifFalse: [ ok := false ] ].
        ok ].
    t check: 'includesKey mid' that: [ d includesKey: 150 ].
    t check: 'not includesKey 201' that: [ (d includesKey: 201) = false ].

    "---- keys / values (keys -> Set, values -> Bag) ---------------------------"
    t check: 'keys size 200' equals: 200 is: [ d keys size ].
    t check: 'keys includes 150' that: [ d keys includes: 150 ].
    t check: 'values size 200' equals: 200 is: [ d values size ].
    t check: 'values includes 1500' that: [ d values includes: 1500 ].

    "---- enumeration ----------------------------------------------------------"
    sum := 0. cnt := 0.
    d keysAndValuesDo: [:k :v | sum := sum + v. cnt := cnt + 1 ].
    t check: 'keysAndValuesDo count' equals: 200 is: [ cnt ].
    t check: 'keysAndValuesDo value sum' equals: 201000 is: [ sum ].

    sum := 0. cnt := 0.
    d do: [:v | sum := sum + v. cnt := cnt + 1 ].
    t check: 'do: over values count' equals: 200 is: [ cnt ].
    t check: 'do: over values sum' equals: 201000 is: [ sum ].

    ksum := 0.
    d keysDo: [:k | ksum := ksum + k ].
    t check: 'keysDo key sum' equals: 20100 is: [ ksum ].

    assocSum := 0.
    d associationsDo: [:as | assocSum := assocSum + as key + as value ].
    t check: 'associationsDo key+value sum' equals: 221100 is: [ assocSum ].

    "---- removing + drain to empty --------------------------------------------"
    t check: 'removeKey: answers removed value' equals: 2000 is: [ d removeKey: 200 ].
    t check: 'size drops to 199' equals: 199 is: [ d size ].
    t check: 'removed key gone' that: [ (d includesKey: 200) = false ].
    t check: 'removeKey:ifAbsent: present answers value' equals: 1990 is: [ d removeKey: 199 ifAbsent: [ #na ] ].

    empty := SharedDictionary new.
    1 to: 10 do: [:i | empty at: i put: i ].
    1 to: 10 do: [:i | empty removeKey: i ].
    t check: 'drained isEmpty' that: [ empty isEmpty ].
    t check: 'drained size 0' equals: 0 is: [ empty size ].

    ^t report
]
