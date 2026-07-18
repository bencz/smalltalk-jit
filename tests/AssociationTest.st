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
    t := TestRun named: 'Association'.

    "---- class-side key:value: construction ----"
    t check: 'key:value: sets key' equals: 3 is: [ (Association key: 3 value: 4) key ].
    t check: 'key:value: sets value' equals: 4 is: [ (Association key: 3 value: 4) value ].
    t check: 'key:value: is an Association' that: [
        (Association key: 3 value: 4) isKindOf: Association ].
    t check: 'key:value: key identity preserved' that: [
        | k a | k := Object new. a := Association key: k value: 1. a key == k ].
    t check: 'key:value: value identity preserved' that: [
        | v a | v := Object new. a := Association key: 1 value: v. a value == v ].

    "---- freshly-made (uninitialized) association ----"
    t check: 'new key is nil' that: [ Association new key isNil ].
    t check: 'new value is nil' that: [ Association new value isNil ].

    "---- Object>>-> builds an Association ----"
    t check: '-> builds an Association' that: [ (5 -> 6) isKindOf: Association ].
    t check: '-> key' equals: 5 is: [ (5 -> 6) key ].
    t check: '-> value' equals: 6 is: [ (5 -> 6) value ].
    t check: '-> from symbol receiver key' equals: #a is: [ (#a -> 1) key ].
    t check: '-> allows nil value' that: [ (1 -> nil) value isNil ].
    t check: '-> allows nil key' that: [ (nil -> 2) key isNil ].
    t check: '-> allows nil key, value kept' equals: 2 is: [ (nil -> 2) value ].

    "---- -> is left-associative: 1 -> 2 -> 3 == (1 -> 2) -> 3 ----"
    t check: 'chained -> outer value' equals: 3 is: [ (1 -> 2 -> 3) value ].
    t check: 'chained -> outer key is an Association' that: [
        (1 -> 2 -> 3) key isKindOf: Association ].
    t check: 'chained -> inner key' equals: 1 is: [ (1 -> 2 -> 3) key key ].
    t check: 'chained -> inner value' equals: 2 is: [ (1 -> 2 -> 3) key value ].

    "---- value: mutates value, leaves key ----"
    t check: 'value: updates value' equals: 99 is: [
        | a | a := 1 -> 2. a value: 99. a value ].
    t check: 'value: leaves key unchanged' equals: 1 is: [
        | a | a := 1 -> 2. a value: 99. a key ].
    t check: 'value: can be reassigned repeatedly' equals: 30 is: [
        | a | a := 1 -> 2. a value: 10. a value: 20. a value: 30. a value ].
    t check: 'value: returns the assigned value' equals: 7 is: [
        | a | a := 1 -> 2. a value: 7 ].

    "---- no single-argument key: setter exists (protocol fact) ----"
    t check: 'no key: setter' that: [ (Association new respondsTo: #key:) = false ].

    "---- printString: key->value ----"
    t check: 'printString of integer assoc' equals: '1->2' is: [ (1 -> 2) printString ].
    t check: 'printString of symbol key' equals: '#a->2' is: [ (#a -> 2) printString ].
    t check: 'printString of character key' equals: '$x->9' is: [ ($x -> 9) printString ].
    t check: 'printString of string key quotes it' equals: '''a''->2' is: [ ('a' -> 2) printString ].
    t check: 'printString of nil value' equals: '1->nil' is: [ (1 -> nil) printString ].

    "---- equality is IDENTITY (Association does not define value =) ----"
    t check: 'assoc = itself' that: [ | a | a := 1 -> 2. a = a ].
    t check: 'assoc == itself' that: [ | a | a := 1 -> 2. a == a ].
    t check: 'distinct equal-content assocs are not =' that: [
        ((1 -> 2) = (1 -> 2)) = false ].
    t check: 'distinct equal-content assocs are not ==' that: [
        ((1 -> 2) == (1 -> 2)) = false ].
    t check: 'distinct equal-content assocs are ~=' that: [ (1 -> 2) ~= (1 -> 2) ].
    t check: 'distinct equal-content assocs are ~~' that: [ (1 -> 2) ~~ (1 -> 2) ].

    "---- hash is stable per object and a valid SmallInteger ----"
    t check: 'hash is stable across sends' that: [
        | a | a := 1 -> 2. a hash = a hash ].
    t check: 'identical assoc hashes equal (== => equal hash)' that: [
        | a b | a := 1 -> 2. b := a. a hash = b hash ].
    t check: 'hash is a non-negative integer' that: [
        | h | h := (1 -> 2) hash. (h isKindOf: Integer) and: [ h >= 0 ] ].

    "---- accessors return exactly the stored objects ----"
    t check: 'key returns stored key object' that: [
        | key a | key := 'thekey'. a := Association key: key value: 1. a key == key ].
    t check: 'value returns stored value object' that: [
        | val a | val := #theval. a := Association key: 1 value: val. a value == val ].

    "---- integration: Dictionary stores associations reachable via associationsDo: ----"
    t check: 'associationsDo: exposes matching key/value' that: [
        | d ok | d := Dictionary new. d at: #k put: 42. ok := false.
        d associationsDo: [:a | (a key = #k and: [ a value = 42 ]) ifTrue: [ ok := true ]]. ok ].
    t check: 'stored association is an Association instance' that: [
        | d cls | d := Dictionary new. d at: #k put: 42. cls := false.
        d associationsDo: [:a | cls := a isKindOf: Association]. cls ].

    ^t report
]
