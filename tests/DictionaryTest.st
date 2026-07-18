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

"A key whose hash is a fixed constant so that many distinct instances collide
 into the same open-addressing probe chain. Equality is value-based on id, so
 two instances with the same id are equal (and merge in a Dictionary), while
 instances with different ids are distinct keys that must coexist."
DictCollKey := Object [
    | id |
    class id: anInteger [ ^self new setId: anInteger ]
    setId: anInteger [ id := anInteger. ^self ]
    id [ ^id ]
    hash [ ^7 ]
    = anObject [ ^(anObject isKindOf: DictCollKey) and: [ anObject id = id ] ]
]

[
    | t |
    t := TestRun named: 'Dictionary'.

    "---- empty dictionary ----"
    t check: 'new isEmpty' that: [ Dictionary new isEmpty ].
    t check: 'new notEmpty is false' that: [ Dictionary new notEmpty = false ].
    t check: 'new size 0' equals: 0 is: [ Dictionary new size ].
    t check: 'empty do: never runs' that: [
        | ran | ran := false.
        Dictionary new do: [:v | ran := true]. ran = false ].

    "---- at:put: returns the value, at: retrieves ----"
    t check: 'at:put: returns value' equals: 99 is: [ Dictionary new at: #k put: 99 ].
    t check: 'at: retrieves stored' equals: 99 is: [
        | d | d := Dictionary new. d at: #k put: 99. d at: #k ].
    t check: 'size after one put' equals: 1 is: [
        | d | d := Dictionary new. d at: #k put: 99. d size ].
    t check: 'one element not empty' that: [
        | d | d := Dictionary new. d at: #k put: 1. d isEmpty = false ].

    "---- overwrite same key mutates in place, size unchanged ----"
    t check: 'overwrite updates value' equals: 7 is: [
        | d | d := Dictionary new. d at: #k put: 1. d at: #k put: 7. d at: #k ].
    t check: 'overwrite keeps size 1' equals: 1 is: [
        | d | d := Dictionary new. d at: #k put: 1. d at: #k put: 7. d size ].

    "---- at:ifAbsent: ----"
    t check: 'at:ifAbsent: present' equals: 5 is: [
        | d | d := Dictionary new. d at: #k put: 5. d at: #k ifAbsent: [ -1 ] ].
    t check: 'at:ifAbsent: absent runs block' equals: -1 is: [
        Dictionary new at: #missing ifAbsent: [ -1 ] ].
    t check: 'at:ifAbsent: does not insert' equals: 0 is: [
        | d | d := Dictionary new. d at: #missing ifAbsent: [ -1 ]. d size ].

    "---- at:ifPresent: ----"
    t check: 'at:ifPresent: absent yields nil' that: [
        (Dictionary new at: #missing ifPresent: [:v | v]) isNil ].
    t check: 'at:ifPresent: present yields block result' equals: 50 is: [
        | d | d := Dictionary new. d at: #k put: 5. d at: #k ifPresent: [:v | v * 10] ].

    "---- includesKey: vs includes: (includes: tests VALUES) ----"
    t check: 'includesKey: true' that: [
        | d | d := Dictionary new. d at: #k put: 1. d includesKey: #k ].
    t check: 'includesKey: false' that: [
        | d | d := Dictionary new. d at: #k put: 1. (d includesKey: #other) = false ].
    t check: 'includes: matches VALUE' that: [
        | d | d := Dictionary new. d at: #k put: 42. d includes: 42 ].
    t check: 'includes: does not match KEY' that: [
        | d | d := Dictionary new. d at: #k put: 42. (d includes: #k) = false ].

    "---- removeKey: ----"
    t check: 'removeKey: returns removed value' equals: 42 is: [
        | d | d := Dictionary new. d at: #k put: 42. d removeKey: #k ].
    t check: 'removeKey: shrinks size' equals: 0 is: [
        | d | d := Dictionary new. d at: #k put: 42. d removeKey: #k. d size ].
    t check: 'removeKey: then absent' that: [
        | d | d := Dictionary new. d at: #k put: 42. d removeKey: #k. (d includesKey: #k) = false ].
    t check: 'removeKey:ifAbsent: present returns value' equals: 42 is: [
        | d | d := Dictionary new. d at: #k put: 42. d removeKey: #k ifAbsent: [ -1 ] ].
    t check: 'removeKey:ifAbsent: absent runs block' equals: -1 is: [
        Dictionary new removeKey: #missing ifAbsent: [ -1 ] ].

    "---- error conditions raise NotFoundError ----"
    t expect: 'at: missing raises NotFoundError' raises: NotFoundError in: [
        Dictionary new at: #missing ].
    t expect: 'removeKey: missing raises NotFoundError' raises: NotFoundError in: [
        Dictionary new removeKey: #missing ].
    t expect: 'keyAtValue: missing raises NotFoundError' raises: NotFoundError in: [
        Dictionary new keyAtValue: 999 ].

    "---- add: an Association merges on key, returns an Association ----"
    t check: 'add: association stores value' equals: 10 is: [
        | d | d := Dictionary new. d add: (1 -> 10). d at: 1 ].
    t check: 'add: returns an Association' that: [
        (Dictionary new add: (1 -> 10)) isKindOf: Association ].
    t check: 'add: same key merges (size 1)' equals: 1 is: [
        | d | d := Dictionary new. d add: (1 -> 10). d add: (1 -> 20). d size ].
    t check: 'add: same key updates value' equals: 20 is: [
        | d | d := Dictionary new. d add: (1 -> 10). d add: (1 -> 20). d at: 1 ].

    "---- remove: anAssociation ifAbsent: removes by key ----"
    t check: 'remove:assoc:ifAbsent present' equals: 0 is: [
        | d | d := Dictionary new. d at: 5 put: 50.
        d remove: (5 -> 999) ifAbsent: [ nil ]. d size ].
    t check: 'remove:assoc:ifAbsent absent runs block (side effect)' that: [
        | ran | ran := false.
        Dictionary new remove: (5 -> 1) ifAbsent: [ ran := true ]. ran ].
    t check: 'remove:assoc:ifAbsent absent leaves size' equals: 0 is: [
        Dictionary new remove: (5 -> 1) ifAbsent: [ nil ]; size ].

    "---- keyAtValue: uses identity on values ----"
    t check: 'keyAtValue: finds key' equals: #b is: [
        | d | d := Dictionary new. d at: #a put: 1. d at: #b put: 2. d keyAtValue: 2 ].
    t check: 'keyAtValue:ifAbsent: absent' equals: #x is: [
        Dictionary new keyAtValue: 7 ifAbsent: [ #x ] ].

    "---- keys is a Set of the keys, values is a Bag of the values ----"
    t check: 'keys is a Set' that: [
        (Dictionary new at: #a put: 1; yourself) keys isKindOf: Set ].
    t check: 'values is a Bag' that: [
        (Dictionary new at: #a put: 1; yourself) values isKindOf: Bag ].
    t check: 'keys size = dict size' equals: 3 is: [
        | d | d := Dictionary new. d at: #a put: 1; at: #b put: 2; at: #c put: 3.
        d keys size ].
    t check: 'keys contains every key' that: [
        | d k | d := Dictionary new. d at: #a put: 1; at: #b put: 2; at: #c put: 3.
        k := d keys. (k includes: #a) and: [ (k includes: #b) and: [ k includes: #c ] ] ].
    t check: 'values contains every value' that: [
        | d v | d := Dictionary new. d at: #a put: 10; at: #b put: 20.
        v := d values. (v includes: 10) and: [ v includes: 20 ] ].
    t check: 'values bag counts duplicate values' equals: 2 is: [
        | d | d := Dictionary new. d at: #a put: 5; at: #b put: 5.
        (d values) occurrencesOf: 5 ].

    "---- enumeration protocols ----"
    t check: 'do: iterates values (sum)' equals: 60 is: [
        | d sum | d := Dictionary new. d at: #a put: 10; at: #b put: 20; at: #c put: 30.
        sum := 0. d do: [:v | sum := sum + v]. sum ].
    t check: 'keysDo: visits each key once' equals: 3 is: [
        | d n | d := Dictionary new. d at: #a put: 1; at: #b put: 2; at: #c put: 3.
        n := 0. d keysDo: [:k | n := n + 1]. n ].
    t check: 'associationsDo: exposes key and value' equals: 33 is: [
        | d s | d := Dictionary new. d at: 1 put: 100; at: 2 put: 200.
        s := 0. d associationsDo: [:a | s := s + a key]. s + 30 ].
    t check: 'keysAndValuesDo: keys+values' equals: 66 is: [
        | d s | d := Dictionary new. d at: 1 put: 10; at: 2 put: 20; at: 3 put: 30.
        s := 0. d keysAndValuesDo: [:k :v | s := s + k + v]. s ].

    "---- select: / reject: / collect: / detect:ifNone: operate on VALUES ----"
    t check: 'select: keeps matching values' equals: 2 is: [
        | d | d := Dictionary new. d at: #a put: 1; at: #b put: 2; at: #c put: 3; at: #d put: 4.
        (d select: [:v | v even]) size ].
    t check: 'reject: drops matching values' equals: 2 is: [
        | d | d := Dictionary new. d at: #a put: 1; at: #b put: 2; at: #c put: 3; at: #d put: 4.
        (d reject: [:v | v even]) size ].
    t check: 'collect: transforms values' equals: 60 is: [
        | d | d := Dictionary new. d at: #a put: 10; at: #b put: 20.
        (d collect: [:v | v * 2]) inject: 0 into: [:a :b | a + b] ].
    t check: 'detect:ifNone: finds a value' equals: 20 is: [
        | d | d := Dictionary new. d at: #a put: 20. d detect: [:v | v > 5] ifNone: [ -1 ] ].
    t check: 'detect:ifNone: none path' equals: -1 is: [
        | d | d := Dictionary new. d at: #a put: 1. d detect: [:v | v > 5] ifNone: [ -1 ] ].

    "---- copy is a distinct object with the same contents ----"
    t check: 'copy is not identical' that: [
        | d | d := Dictionary new. d at: #a put: 1. (d copy == d) = false ].
    t check: 'copy has same size' equals: 2 is: [
        | d | d := Dictionary new. d at: #a put: 1; at: #b put: 2. d copy size ].
    t check: 'copy keys retrievable' equals: 1 is: [
        | d | d := Dictionary new. d at: #a put: 1. (d copy) at: #a ].

    "---- identity equality (Dictionary does not define value =) ----"
    t check: 'dict = itself' that: [
        | d | d := Dictionary new. d at: #a put: 1. d = d ].
    t check: 'distinct equal-content dicts are not =' that: [
        | a b | a := Dictionary new. a at: #k put: 1.
        b := Dictionary new. b at: #k put: 1. (a = b) = false ].

    "---- heterogeneous key types coexist ----"
    t check: 'mixed key types stored' equals: 4 is: [
        | d | d := Dictionary new.
        d at: #sym put: 1. d at: 'str' put: 2. d at: 42 put: 3. d at: $c put: 4.
        d size ].
    t check: 'symbol key retrievable' equals: 1 is: [
        | d | d := Dictionary new. d at: #sym put: 1. d at: 'str' put: 2. d at: 42 put: 3.
        d at: #sym ].
    t check: 'string key retrievable' equals: 2 is: [
        | d | d := Dictionary new. d at: #sym put: 1. d at: 'str' put: 2. d at: 42 put: 3.
        d at: 'str' ].
    t check: 'integer key retrievable' equals: 3 is: [
        | d | d := Dictionary new. d at: #sym put: 1. d at: 'str' put: 2. d at: 42 put: 3.
        d at: 42 ].
    t check: 'value-equal string keys map to one entry' equals: 1 is: [
        | d | d := Dictionary new. d at: 'abc' put: 1. d at: ('ab', 'c') put: 2. d size ].
    t check: 'value-equal string keys share value' equals: 2 is: [
        | d | d := Dictionary new. d at: 'abc' put: 1. d at: ('ab', 'c') put: 2. d at: 'abc' ].
    t check: 'equal keys hash equal (String)' that: [ 'abc' hash = ('ab', 'c') hash ].
    t check: 'equal keys hash equal (Symbol)' that: [ #foo hash = #foo hash ].
    t check: 'equal keys hash equal (SmallInteger)' that: [ (2 + 1) hash = 3 hash ].

    "---- GROWTH: force at least one rehash and verify integrity ----"
    t check: 'grow: size after 200 inserts' equals: 200 is: [
        | d | d := Dictionary new. 1 to: 200 do: [:i | d at: i put: i * i]. d size ].
    t check: 'grow: all keys still retrievable' that: [
        | d ok | d := Dictionary new. 1 to: 200 do: [:i | d at: i put: i * i].
        ok := true. 1 to: 200 do: [:i | (d at: i) = (i * i) ifFalse: [ ok := false ]]. ok ].
    t check: 'grow: keys set complete after rehash' equals: 200 is: [
        | d | d := Dictionary new. 1 to: 200 do: [:i | d at: i put: i]. d keys size ].
    t check: 'grow: missing key still absent after grow' that: [
        | d | d := Dictionary new. 1 to: 200 do: [:i | d at: i put: i].
        (d includesKey: 9999) = false ].

    "---- COLLISIONS: many keys sharing one hash must coexist ----"
    t check: 'collision keys all coexist (size)' equals: 20 is: [
        | d | d := Dictionary new.
        1 to: 20 do: [:i | d at: (DictCollKey id: i) put: i * 100]. d size ].
    t check: 'collision keys all retrievable' that: [
        | d ok | d := Dictionary new.
        1 to: 20 do: [:i | d at: (DictCollKey id: i) put: i * 100].
        ok := true.
        1 to: 20 do: [:i | (d at: (DictCollKey id: i)) = (i * 100) ifFalse: [ ok := false ]]. ok ].
    t check: 'equal collision key overwrites, not adds' equals: 1 is: [
        | d | d := Dictionary new.
        d at: (DictCollKey id: 1) put: 10. d at: (DictCollKey id: 1) put: 20. d size ].
    t check: 'equal collision key latest value' equals: 20 is: [
        | d | d := Dictionary new.
        d at: (DictCollKey id: 1) put: 10. d at: (DictCollKey id: 1) put: 20.
        d at: (DictCollKey id: 1) ].
    t check: 'removing mid-chain keeps others reachable' that: [
        | d ok | d := Dictionary new.
        1 to: 20 do: [:i | d at: (DictCollKey id: i) put: i].
        d removeKey: (DictCollKey id: 10).
        ok := (d includesKey: (DictCollKey id: 10)) = false.
        1 to: 20 do: [:i | i = 10 ifFalse: [
            (d at: (DictCollKey id: i) ifAbsent: [ -1 ]) = i ifFalse: [ ok := false ]]]. ok ].
    t check: 'size after mid-chain removal' equals: 19 is: [
        | d | d := Dictionary new.
        1 to: 20 do: [:i | d at: (DictCollKey id: i) put: i].
        d removeKey: (DictCollKey id: 10). d size ].

    ^t report
]
