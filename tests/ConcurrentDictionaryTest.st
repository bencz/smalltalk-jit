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

"=== ConcurrentDictionary: read-mostly copy-on-write map. Correctness driven
 from a SINGLE fiber (no workers). Covers construction, accessing, testing,
 removing, enumeration, and growth well past the initial capacity so the
 copy-on-write rebuild path is exercised. ==="

[
    | t d calls r sum cnt ksum missing empty assocSum |
    t := TestRun named: 'ConcurrentDictionary'.

    "---- construction + empty state -------------------------------------------"
    d := ConcurrentDictionary new.
    t check: 'new isEmpty' that: [ d isEmpty ].
    t check: 'new notEmpty is false' that: [ d notEmpty = false ].
    t check: 'new size 0' equals: 0 is: [ d size ].
    t check: 'new: capacity isEmpty' that: [ (ConcurrentDictionary new: 5) isEmpty ].

    "---- at:put: / at: / includesKey: -----------------------------------------"
    t check: 'at:put: answers stored value' equals: 42 is: [ d at: #a put: 42 ].
    t check: 'at: retrieves' equals: 42 is: [ d at: #a ].
    t check: 'includesKey present' that: [ d includesKey: #a ].
    t check: 'includesKey absent false' that: [ (d includesKey: #zzz) = false ].
    t check: 'size grows to 1' equals: 1 is: [ d size ].
    t check: 'notEmpty after put' that: [ d notEmpty ].

    "overwriting an existing key updates value, keeps size"
    d at: #a put: 43.
    t check: 'overwrite updates value' equals: 43 is: [ d at: #a ].
    t check: 'overwrite keeps size 1' equals: 1 is: [ d size ].

    "---- at:ifAbsent: ----------------------------------------------------------"
    t check: 'at:ifAbsent: present returns value' equals: 43 is: [ d at: #a ifAbsent: [ #dflt ] ].
    t check: 'at:ifAbsent: absent runs block' equals: #dflt is: [ d at: #missing ifAbsent: [ #dflt ] ].
    t check: 'at:ifAbsent: did not insert' that: [ (d includesKey: #missing) = false ].

    "---- at:ifPresent: ---------------------------------------------------------"
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

    "---- missing-key errors (receiver bound to a temp: safe) -------------------"
    missing := ConcurrentDictionary new.
    t expect: 'at: on missing raises NotFoundError' raises: NotFoundError in: [ missing at: #ghost ].
    t expect: 'removeKey: on missing raises NotFoundError' raises: NotFoundError in: [ missing removeKey: #ghost ].
    t check: 'removeKey:ifAbsent: absent runs block' equals: #gone is: [ missing removeKey: #x ifAbsent: [ #gone ] ].

    "---- bulk insert past initial capacity (growth / copy-on-write) ------------"
    d := ConcurrentDictionary new.
    1 to: 200 do: [:i | d at: i put: i * 10 ].
    t check: 'bulk size 200' equals: 200 is: [ d size ].
    t check: 'all 200 keys retrievable' that: [
        | ok | ok := true.
        1 to: 200 do: [:i | (d at: i) = (i * 10) ifFalse: [ ok := false ] ].
        ok ].
    t check: 'first key retrievable' equals: 10 is: [ d at: 1 ].
    t check: 'last key retrievable' equals: 2000 is: [ d at: 200 ].
    t check: 'includesKey mid' that: [ d includesKey: 150 ].
    t check: 'not includesKey 201' that: [ (d includesKey: 201) = false ].

    "---- keys / values (keys -> Set, values -> Bag) ---------------------------"
    t check: 'keys size 200' equals: 200 is: [ d keys size ].
    t check: 'keys includes 150' that: [ d keys includes: 150 ].
    t check: 'values size 200' equals: 200 is: [ d values size ].
    t check: 'values includes 1500' that: [ d values includes: 1500 ].

    "---- enumeration: keysAndValuesDo: ----------------------------------------"
    sum := 0. cnt := 0.
    d keysAndValuesDo: [:k :v | sum := sum + v. cnt := cnt + 1 ].
    t check: 'keysAndValuesDo count' equals: 200 is: [ cnt ].
    t check: 'keysAndValuesDo value sum' equals: 201000 is: [ sum ].

    "---- enumeration: do: (over values) ---------------------------------------"
    sum := 0. cnt := 0.
    d do: [:v | sum := sum + v. cnt := cnt + 1 ].
    t check: 'do: count' equals: 200 is: [ cnt ].
    t check: 'do: value sum' equals: 201000 is: [ sum ].

    "---- enumeration: keysDo: -------------------------------------------------"
    ksum := 0.
    d keysDo: [:k | ksum := ksum + k ].
    t check: 'keysDo key sum' equals: 20100 is: [ ksum ].

    "---- enumeration: associationsDo: -----------------------------------------"
    assocSum := 0.
    d associationsDo: [:as | assocSum := assocSum + as key + as value ].
    t check: 'associationsDo key+value sum' equals: 221100 is: [ assocSum ].

    "---- removeKey: after growth ----------------------------------------------"
    t check: 'removeKey: answers removed value' equals: 2000 is: [ d removeKey: 200 ].
    t check: 'size drops to 199' equals: 199 is: [ d size ].
    t check: 'removed key gone' that: [ (d includesKey: 200) = false ].
    t check: 'removeKey:ifAbsent: present answers value' equals: 1990 is: [ d removeKey: 199 ifAbsent: [ #na ] ].
    t check: 'size drops to 198' equals: 198 is: [ d size ].

    "---- drain to empty --------------------------------------------------------"
    empty := ConcurrentDictionary new.
    1 to: 10 do: [:i | empty at: i put: i ].
    1 to: 10 do: [:i | empty removeKey: i ].
    t check: 'drained isEmpty' that: [ empty isEmpty ].
    t check: 'drained size 0' equals: 0 is: [ empty size ].

    ^t report
]
