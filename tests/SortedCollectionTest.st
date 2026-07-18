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

"=== SortedCollectionTest: exercises SortedCollection - default ascending
 insertion order, custom sortBlock (descending), duplicates, addAll:, first/
 last/at:/size/includes:, removeFirst/removeLast/remove:, conversions, equality
 (class-sensitive), and that addFirst:/addLast:/collect: honour the sort order. ==="

[
    | t sc desc dups |
    t := TestRun named: 'SortedCollection'.

    "---- default ascending order ----"
    sc := SortedCollection new.
    sc add: 5; add: 1; add: 3; add: 2; add: 4.
    t check: 'add out of order yields ascending' equals: #(1 2 3 4 5) is: [ sc asArray ].
    t check: 'add: returns the added object' equals: 42 is: [ SortedCollection new add: 42 ].
    t check: 'class is SortedCollection' equals: SortedCollection is: [ sc class ].
    t check: 'size' equals: 5 is: [ sc size ].
    t check: 'isEmpty on new' that: [ SortedCollection new isEmpty ].
    t check: 'notEmpty after add' that: [ sc isEmpty not ].

    "---- addAll: keeps order, even growing past initial capacity ----"
    t check: 'addAll: sorts' equals: #(1 3 5 7 9) is: [
        (SortedCollection new addAll: #(9 3 7 1 5); yourself) asArray ].
    t check: 'addAll: many stays sorted' equals: ((1 to: 40) asArray) is: [
        (SortedCollection new addAll: (40 to: 1 by: -1); yourself) asArray ].

    "---- accessing ----"
    t check: 'first is minimum' equals: 1 is: [ sc first ].
    t check: 'last is maximum' equals: 5 is: [ sc last ].
    t check: 'at: 1 is first' equals: 1 is: [ sc at: 1 ].
    t check: 'at: size is last' equals: 5 is: [ sc at: sc size ].
    t check: 'at: middle' equals: 3 is: [ sc at: 3 ].
    t check: 'indexOf: gives sorted position' equals: 4 is: [ sc indexOf: 4 ].
    t check: 'includes: present' that: [ sc includes: 3 ].
    t check: 'includes: absent' that: [ (sc includes: 99) not ].

    "---- duplicates are kept, in order ----"
    dups := SortedCollection new addAll: #(2 1 2 1 3); yourself.
    t check: 'duplicates preserved and sorted' equals: #(1 1 2 2 3) is: [ dups asArray ].
    t check: 'occurrencesOf: counts duplicates' equals: 2 is: [ dups occurrencesOf: 2 ].

    "---- custom sortBlock: descending ----"
    desc := SortedCollection sortBlock: [ :a :b | a >= b ].
    desc add: 1; add: 5; add: 3; add: 9; add: 7.
    t check: 'custom descending order' equals: #(9 7 5 3 1) is: [ desc asArray ].
    t check: 'descending first is maximum' equals: 9 is: [ desc first ].
    t check: 'descending last is minimum' equals: 1 is: [ desc last ].
    t check: 'asSortedCollection: block descending' equals: #(5 4 3 2 1) is: [
        (#(3 1 5 2 4) asSortedCollection: [ :a :b | a >= b ]) asArray ].

    "---- addFirst:/addLast: still insert by sort order, not at the ends ----"
    t check: 'addFirst: inserts in sorted position not at front' equals: #(2 4 5 6) is: [
        (SortedCollection new addAll: #(2 4 6); addFirst: 5; yourself) asArray ].
    t check: 'addLast: inserts in sorted position not at back' equals: #(2 3 4 6) is: [
        (SortedCollection new addAll: #(2 4 6); addLast: 3; yourself) asArray ].

    "---- removing ----"
    t check: 'removeFirst returns minimum' equals: 1 is: [
        (SortedCollection new addAll: #(3 1 2); yourself) removeFirst ].
    t check: 'removeFirst leaves the rest sorted' equals: #(2 3) is: [
        | s | s := SortedCollection new addAll: #(3 1 2); yourself. s removeFirst. s asArray ].
    t check: 'removeLast returns maximum' equals: 3 is: [
        (SortedCollection new addAll: #(3 1 2); yourself) removeLast ].
    t check: 'remove: keeps order' equals: #(1 3) is: [
        | s | s := SortedCollection new addAll: #(3 1 2); yourself. s remove: 2. s asArray ].
    t expect: 'first on empty raises' raises: OutOfRangeError in: [ SortedCollection new first ].
    t expect: 'removeFirst on empty raises' raises: OutOfRangeError in: [ SortedCollection new removeFirst ].

    "---- conversion / reducing ----"
    t check: 'asSortedCollection from Array' equals: #(1 2 3 5 8) is: [ #(5 3 8 1 2) asSortedCollection asArray ].
    t check: 'withAll: sorts' equals: #(1 2 3) is: [ (SortedCollection withAll: #(3 1 2)) asArray ].
    t check: 'with:with: sorts' equals: #(1 3) is: [ (SortedCollection with: 3 with: 1) asArray ].
    t check: 'sum over sorted' equals: 15 is: [ sc sum ].
    t check: 'inject:into: over sorted' equals: 120 is: [ sc inject: 1 into: [ :p :x | p * x ] ].
    t check: 'reverse yields descending elements' equals: #(5 4 3 2 1) is: [ sc reverse asArray ].

    "---- collect: honours the sort order of the result ----"
    t check: 'collect: returns a SortedCollection' equals: SortedCollection is: [ (sc collect: [ :x | x ]) class ].
    t check: 'collect: re-sorts even when mapping reverses order' equals: #(6 7 8 9 10) is: [
        (sc collect: [ :x | 11 - x ]) asArray ].

    "---- equality is class- and element-sensitive ----"
    t check: 'equal SortedCollections =' that: [
        (SortedCollection withAll: #(3 1 2)) = (SortedCollection withAll: #(1 2 3)) ].
    t check: 'different elements not =' that: [
        ((SortedCollection withAll: #(1 2 3)) = (SortedCollection withAll: #(1 2 4))) not ].
    t check: 'not equal to OrderedCollection with same elements' that: [
        ((SortedCollection withAll: #(1 2 3)) = (OrderedCollection withAll: #(1 2 3))) not ].

    ^t report
]
