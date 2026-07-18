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

[
    | t |
    t := TestRun named: 'Bag'.

    "---- empty bag ----"
    t check: 'new isEmpty' that: [ Bag new isEmpty ].
    t check: 'new notEmpty false' that: [ Bag new notEmpty = false ].
    t check: 'new size 0' equals: 0 is: [ Bag new size ].
    t check: 'empty includes: false' that: [ (Bag new includes: #x) = false ].
    t check: 'empty occurrencesOf: 0' equals: 0 is: [ Bag new occurrencesOf: #x ].
    t check: 'empty do: never runs' that: [
        | ran | ran := false. Bag new do: [:v | ran := true]. ran = false ].

    "---- add: single ----"
    t check: 'add: returns argument' equals: #x is: [ Bag new add: #x ].
    t check: 'add: makes size 1' equals: 1 is: [ | b | b := Bag new. b add: #x. b size ].
    t check: 'add: then includes:' that: [ | b | b := Bag new. b add: #x. b includes: #x ].
    t check: 'add: occurrencesOf 1' equals: 1 is: [
        | b | b := Bag new. b add: #x. b occurrencesOf: #x ].

    "---- duplicates raise the count, not distinct membership ----"
    t check: 'adding same twice size 2' equals: 2 is: [
        | b | b := Bag new. b add: #x. b add: #x. b size ].
    t check: 'adding same twice occurrences 2' equals: 2 is: [
        | b | b := Bag new. b add: #x. b add: #x. b occurrencesOf: #x ].
    t check: 'adding same still one distinct member' equals: 1 is: [
        | b | b := Bag new. b add: #x; add: #x; add: #x. b asSet size ].

    "---- add:withOccurrences: ----"
    t check: 'add:withOccurrences: returns arg' equals: #y is: [
        Bag new add: #y withOccurrences: 5 ].
    t check: 'add:withOccurrences: sets count' equals: 5 is: [
        | b | b := Bag new. b add: #y withOccurrences: 5. b occurrencesOf: #y ].
    t check: 'add:withOccurrences: sets size' equals: 5 is: [
        | b | b := Bag new. b add: #y withOccurrences: 5. b size ].
    t check: 'withOccurrences: accumulates with add:' equals: 6 is: [
        | b | b := Bag new. b add: #y withOccurrences: 5. b add: #y. b occurrencesOf: #y ].
    t check: 'withOccurrences: twice accumulates' equals: 7 is: [
        | b | b := Bag new. b add: #y withOccurrences: 3. b add: #y withOccurrences: 4.
        b occurrencesOf: #y ].

    "---- size counts total occurrences across members ----"
    t check: 'size sums occurrences of all members' equals: 5 is: [
        | b | b := Bag new. b add: #a withOccurrences: 2. b add: #b withOccurrences: 3. b size ].
    t check: 'occurrencesOf never-added is 0' equals: 0 is: [
        | b | b := Bag new. b add: #a. b occurrencesOf: #never ].
    t check: 'includes: distinct members' that: [
        | b | b := Bag new. b add: #a; add: #b. (b includes: #a) and: [ b includes: #b ] ].

    "---- remove: decrements, drops member at zero ----"
    t check: 'remove: returns the value' equals: #x is: [
        | b | b := Bag new. b add: #x withOccurrences: 2. b remove: #x ].
    t check: 'remove: decrements count' equals: 1 is: [
        | b | b := Bag new. b add: #x withOccurrences: 2. b remove: #x. b occurrencesOf: #x ].
    t check: 'remove: decrements size' equals: 1 is: [
        | b | b := Bag new. b add: #x withOccurrences: 2. b remove: #x. b size ].
    t check: 'remove: last drops membership' that: [
        | b | b := Bag new. b add: #x. b remove: #x. (b includes: #x) = false ].
    t check: 'remove: last size 0' equals: 0 is: [
        | b | b := Bag new. b add: #x. b remove: #x. b size ].
    t check: 'remove:ifAbsent: present' equals: #x is: [
        | b | b := Bag new. b add: #x. b remove: #x ifAbsent: [ #none ] ].
    t check: 'remove:ifAbsent: absent runs block' equals: #none is: [
        Bag new remove: #x ifAbsent: [ #none ] ].
    t expect: 'remove: absent raises NotFoundError' raises: NotFoundError in: [
        Bag new remove: #x ].

    "---- do: yields each element once per occurrence ----"
    t check: 'do: total visits = size' equals: 5 is: [
        | b n | b := Bag new. b add: #a withOccurrences: 2. b add: #b withOccurrences: 3.
        n := 0. b do: [:v | n := n + 1]. n ].
    t check: 'do: repeats a member by its count' equals: 3 is: [
        | b n | b := Bag new. b add: #a withOccurrences: 3.
        n := 0. b do: [:v | v = #a ifTrue: [ n := n + 1 ]]. n ].
    t check: 'do: sum over integer members with counts' equals: 13 is: [
        | b sum | b := Bag new. b add: 2 withOccurrences: 2. b add: 3 withOccurrences: 3.
        sum := 0. b do: [:v | sum := sum + v]. sum ].

    "---- conversions ----"
    t check: 'asSet collapses to distinct members' equals: 2 is: [
        | b | b := Bag new. b add: #a withOccurrences: 4. b add: #b withOccurrences: 1.
        b asSet size ].
    t check: 'asSet membership preserved' that: [
        | b s | b := Bag new. b add: #a withOccurrences: 4. b add: #b.
        s := b asSet. (s includes: #a) and: [ s includes: #b ] ].
    t check: 'asArray length equals size (with repeats)' equals: 4 is: [
        | b | b := Bag new. b add: #a withOccurrences: 3. b add: #b. b asArray size ].
    t check: 'withAll: counts repeats in source' equals: 3 is: [
        (Bag withAll: #(7 7 7)) occurrencesOf: 7 ].
    t check: 'withAll: total size' equals: 5 is: [
        (Bag withAll: #(1 1 2 3 3)) size ].

    "---- enumeration helpers inherited from Collection ----"
    t check: 'select: keeps matching occurrences' equals: 3 is: [
        | b | b := Bag new. b add: 2 withOccurrences: 2. b add: 5 withOccurrences: 3.
        (b select: [:v | v odd]) size ].
    t check: 'collect: maps every occurrence' equals: 4 is: [
        | b | b := Bag new. b add: #a withOccurrences: 3. b add: #b.
        (b collect: [:v | v]) size ].
    t check: 'count: counts occurrences that match' equals: 2 is: [
        | b | b := Bag new. b add: 4 withOccurrences: 2. b add: 5 withOccurrences: 3.
        b count: [:v | v even] ].
    t check: 'occurrencesOf via Collection matches count' equals: 3 is: [
        | b | b := Bag new. b add: #a withOccurrences: 3. b add: #b.
        b occurrencesOf: #a ].

    "---- identity equality (Bag does not define value =) ----"
    t check: 'bag = itself' that: [ | b | b := Bag new. b add: #a. b = b ].
    t check: 'distinct equal-content bags are not =' that: [
        | a c | a := Bag new. a add: #x. c := Bag new. c add: #x. (a = c) = false ].

    "---- larger bag: many members and counts stay consistent ----"
    t check: 'many members total size' equals: 5050 is: [
        | b | b := Bag new. 1 to: 100 do: [:i | b add: i withOccurrences: i]. b size ].
    t check: 'many members distinct count' equals: 100 is: [
        | b | b := Bag new. 1 to: 100 do: [:i | b add: i withOccurrences: i]. b asSet size ].
    t check: 'occurrences of a specific member' equals: 42 is: [
        | b | b := Bag new. 1 to: 100 do: [:i | b add: i withOccurrences: i]. b occurrencesOf: 42 ].

    ^t report
]
