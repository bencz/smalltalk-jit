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

"A member whose hash is a fixed constant so distinct instances collide into the
 same open-addressing probe chain. Equality is value-based on id."
SetCollKey := Object [
    | id |
    class id: anInteger [ ^self new setId: anInteger ]
    setId: anInteger [ id := anInteger. ^self ]
    id [ ^id ]
    hash [ ^13 ]
    = anObject [ ^(anObject isKindOf: SetCollKey) and: [ anObject id = id ] ]
]

[
    | t |
    t := TestRun named: 'Set'.

    "---- empty set ----"
    t check: 'new isEmpty' that: [ Set new isEmpty ].
    t check: 'new notEmpty false' that: [ Set new notEmpty = false ].
    t check: 'new size 0' equals: 0 is: [ Set new size ].
    t check: 'empty includes: false' that: [ (Set new includes: 1) = false ].
    t check: 'empty do: never runs' that: [
        | ran | ran := false. Set new do: [:v | ran := true]. ran = false ].

    "---- add: returns the argument, one element ----"
    t check: 'add: returns argument' equals: 7 is: [ Set new add: 7 ].
    t check: 'add: size 1' equals: 1 is: [ | s | s := Set new. s add: 7. s size ].
    t check: 'add: then includes:' that: [ | s | s := Set new. s add: 7. s includes: 7 ].
    t check: 'not-added not included' that: [ | s | s := Set new. s add: 7. (s includes: 8) = false ].

    "---- duplicates ignored ----"
    t check: 'adding same value twice keeps size 1' equals: 1 is: [
        | s | s := Set new. s add: 5. s add: 5. s size ].
    t check: 'many duplicate adds keep size 1' equals: 1 is: [
        | s | s := Set new. 1 to: 50 do: [:i | s add: 5]. s size ].
    t check: 'distinct adds increase size' equals: 3 is: [
        | s | s := Set new. s add: 1; add: 2; add: 3. s size ].
    t check: 'value-equal strings dedupe' equals: 1 is: [
        | s | s := Set new. s add: 'abc'. s add: ('ab', 'c'). s size ].

    "---- nil is not stored (Set>>add: ignores nil) ----"
    t check: 'add: nil returns nil' that: [ (Set new add: nil) isNil ].
    t check: 'add: nil keeps size 0' equals: 0 is: [ | s | s := Set new. s add: nil. s size ].

    "---- addAll: ----"
    t check: 'addAll: from array' equals: 3 is: [
        | s | s := Set new. s addAll: #(1 2 3). s size ].
    t check: 'addAll: returns its argument' equals: 4 is: [
        | s r | s := Set new. r := s addAll: #(1 2 3 4). r size ].
    t check: 'addAll: dedupes across sources' equals: 3 is: [
        | s | s := Set new. s addAll: #(1 2 2 3 3 3). s size ].
    t check: 'withAll: builds a populated set' equals: 3 is: [
        (Set withAll: #(9 8 7)) size ].
    t check: 'withAll: dedupes' equals: 2 is: [
        (Set withAll: #(1 1 2 2)) size ].

    "---- remove: / remove:ifAbsent: ----"
    t check: 'remove: returns the value' equals: 5 is: [
        | s | s := Set new. s add: 5. s remove: 5 ].
    t check: 'remove: shrinks size' equals: 0 is: [
        | s | s := Set new. s add: 5. s remove: 5. s size ].
    t check: 'remove: makes it absent' that: [
        | s | s := Set new. s add: 5. s remove: 5. (s includes: 5) = false ].
    t check: 'remove:ifAbsent: present' equals: 5 is: [
        | s | s := Set new. s add: 5. s remove: 5 ifAbsent: [ -1 ] ].
    t check: 'remove:ifAbsent: absent runs block' equals: -1 is: [
        Set new remove: 99 ifAbsent: [ -1 ] ].
    t expect: 'remove: absent raises NotFoundError' raises: NotFoundError in: [
        Set new remove: 99 ].
    t check: 'removeAll: removes each' equals: 1 is: [
        | s | s := Set new. s addAll: #(1 2 3). s removeAll: #(1 2). s size ].

    "---- do: visits every distinct element exactly once ----"
    t check: 'do: element count' equals: 4 is: [
        | s n | s := Set new. s addAll: #(1 2 3 4). n := 0. s do: [:v | n := n + 1]. n ].
    t check: 'do: sum of elements' equals: 10 is: [
        | s sum | s := Set new. s addAll: #(1 2 3 4). sum := 0. s do: [:v | sum := sum + v]. sum ].
    t check: 'do: over duplicates visits once' equals: 1 is: [
        | s n | s := Set new. s add: 9; add: 9; add: 9. n := 0. s do: [:v | n := n + 1]. n ].

    "---- select: / reject: / collect: return OrderedCollections ----"
    t check: 'select: filters' equals: 2 is: [
        | s | s := Set new. s addAll: #(1 2 3 4). (s select: [:v | v even]) size ].
    t check: 'reject: filters' equals: 2 is: [
        | s | s := Set new. s addAll: #(1 2 3 4). (s reject: [:v | v even]) size ].
    t check: 'collect: maps each element' equals: 4 is: [
        | s | s := Set new. s addAll: #(1 2 3 4). (s collect: [:v | v * v]) size ].
    t check: 'detect:ifNone: finds' that: [
        | s | s := Set new. s addAll: #(1 2 3). (s detect: [:v | v = 2] ifNone: [ -1 ]) = 2 ].
    t check: 'detect:ifNone: none' equals: -1 is: [
        | s | s := Set new. s addAll: #(1 2 3). s detect: [:v | v = 9] ifNone: [ -1 ] ].
    t check: 'count: matching' equals: 2 is: [
        | s | s := Set new. s addAll: #(1 2 3 4). s count: [:v | v even] ].

    "---- conversions ----"
    t check: 'asArray size' equals: 3 is: [
        | s | s := Set new. s addAll: #(1 2 3). s asArray size ].
    t check: 'asArray contains all' that: [
        | s a | s := Set new. s addAll: #(1 2 3). a := s asArray.
        (a includes: 1) and: [ (a includes: 2) and: [ a includes: 3 ] ] ].
    t check: 'asSet of a Set has same size' equals: 3 is: [
        | s | s := Set new. s addAll: #(1 2 3). s asSet size ].
    t check: 'asBag counts singletons' equals: 1 is: [
        | s | s := Set new. s addAll: #(1 2 3). (s asBag) occurrencesOf: 1 ].

    "---- identity equality (Set does not define value =) ----"
    t check: 'set = itself' that: [ | s | s := Set new. s add: 1. s = s ].
    t check: 'distinct equal-content sets are not =' that: [
        | a b | a := Set new. a add: 1. b := Set new. b add: 1. (a = b) = false ].

    "---- heterogeneous element types coexist ----"
    t check: 'mixed types stored distinctly' equals: 4 is: [
        | s | s := Set new. s add: #sym; add: 'str'; add: 42; add: $c. s size ].
    t check: 'symbol member present' that: [
        | s | s := Set new. s add: #sym; add: 42. s includes: #sym ].

    "---- GROWTH: past initial capacity, integrity preserved ----"
    t check: 'grow: size after 150 distinct adds' equals: 150 is: [
        | s | s := Set new. 1 to: 150 do: [:i | s add: i]. s size ].
    t check: 'grow: all members present after rehash' that: [
        | s ok | s := Set new. 1 to: 150 do: [:i | s add: i].
        ok := true. 1 to: 150 do: [:i | (s includes: i) ifFalse: [ ok := false ]]. ok ].
    t check: 'grow: no phantom members' that: [
        | s | s := Set new. 1 to: 150 do: [:i | s add: i]. (s includes: 9999) = false ].
    t check: 'grow: asArray length matches size' equals: 150 is: [
        | s | s := Set new. 1 to: 150 do: [:i | s add: i]. s asArray size ].

    "---- COLLISIONS: distinct members sharing one hash coexist ----"
    t check: 'collision members all coexist' equals: 20 is: [
        | s | s := Set new. 1 to: 20 do: [:i | s add: (SetCollKey id: i)]. s size ].
    t check: 'collision members all present' that: [
        | s ok | s := Set new. 1 to: 20 do: [:i | s add: (SetCollKey id: i)].
        ok := true. 1 to: 20 do: [:i | (s includes: (SetCollKey id: i)) ifFalse: [ ok := false ]]. ok ].
    t check: 'equal collision member deduped' equals: 1 is: [
        | s | s := Set new. s add: (SetCollKey id: 1). s add: (SetCollKey id: 1). s size ].
    t check: 'removing mid-chain keeps others present' that: [
        | s ok | s := Set new. 1 to: 20 do: [:i | s add: (SetCollKey id: i)].
        s remove: (SetCollKey id: 10).
        ok := (s includes: (SetCollKey id: 10)) = false.
        1 to: 20 do: [:i | i = 10 ifFalse: [
            (s includes: (SetCollKey id: i)) ifFalse: [ ok := false ]]]. ok ].
    t check: 'size after mid-chain removal' equals: 19 is: [
        | s | s := Set new. 1 to: 20 do: [:i | s add: (SetCollKey id: i)].
        s remove: (SetCollKey id: 10). s size ].

    ^t report
]
