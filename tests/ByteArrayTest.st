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

"=== ByteArrayTest: exercises ByteArray - zero-filled allocation, byte at:/at:put:
 with 0..255, out-of-bounds errors, equality (class-sensitive), conversions,
 enumeration, copying and concatenation. NOTE: this VM does NOT validate the byte
 value on at:put:; it silently masks to the low 8 bits (see report). ==="

[
    | t ba |
    t := TestRun named: 'ByteArray'.
    ba := ByteArray withAll: #(255 0 128).

    "---- allocation is zero-filled ----"
    t check: 'new: has the requested size' equals: 4 is: [ (ByteArray new: 4) size ].
    t check: 'new: is zero-filled at first slot' equals: 0 is: [ (ByteArray new: 4) at: 1 ].
    t check: 'new: is zero-filled at last slot' equals: 0 is: [ (ByteArray new: 4) at: 4 ].
    t check: 'new: 0 is empty' that: [ (ByteArray new: 0) isEmpty ].
    t check: 'new: 0 size is 0' equals: 0 is: [ (ByteArray new: 0) size ].
    t check: 'class is ByteArray' equals: ByteArray is: [ ba class ].

    "---- at: / at:put: over the full 0..255 range ----"
    t check: 'at: reads stored bytes' equals: 128 is: [ ba at: 3 ].
    t check: 'at:put: 0 boundary' equals: 0 is: [ | b | b := ByteArray new: 1. b at: 1 put: 0. b at: 1 ].
    t check: 'at:put: 255 boundary' equals: 255 is: [ | b | b := ByteArray new: 1. b at: 1 put: 255. b at: 1 ].
    t check: 'at:put: mid value' equals: 200 is: [ | b | b := ByteArray new: 1. b at: 1 put: 200. b at: 1 ].
    t check: 'at:put: overwrites in place' equals: #(9 0 128) is: [
        | b | b := ByteArray withAll: #(255 0 128). b at: 1 put: 9. b asArray ].

    "---- at:put: validates the 0..255 byte range, raising OutOfRangeError outside it ----"
    t expect: 'at:put: 256 raises' raises: OutOfRangeError in: [ | b | b := ByteArray new: 1. b at: 1 put: 256 ].
    t expect: 'at:put: -1 raises' raises: OutOfRangeError in: [ | b | b := ByteArray new: 1. b at: 1 put: -1 ].
    t expect: 'at:put: 300 raises' raises: OutOfRangeError in: [ | b | b := ByteArray new: 1. b at: 1 put: 300 ].
    t expect: 'at:put: non-integer raises' raises: OutOfRangeError in: [ | b | b := ByteArray new: 1. b at: 1 put: $x ].

    "---- out-of-bounds indices raise ----"
    t expect: 'at: index 0 raises' raises: OutOfRangeError in: [ ba at: 0 ].
    t expect: 'at: past end raises' raises: OutOfRangeError in: [ ba at: 4 ].
    t expect: 'at:put: index 0 raises' raises: OutOfRangeError in: [ ba at: 0 put: 1 ].
    t expect: 'at:put: past end raises' raises: OutOfRangeError in: [ ba at: 4 put: 1 ].

    "---- equality is class- and element-sensitive ----"
    t check: 'equal ByteArrays =' that: [ (ByteArray withAll: #(1 2 3)) = (ByteArray withAll: #(1 2 3)) ].
    t check: 'different bytes not =' that: [ ((ByteArray withAll: #(1 2 3)) = (ByteArray withAll: #(1 2 4))) not ].
    t check: 'different size not =' that: [ ((ByteArray withAll: #(1 2 3)) = (ByteArray withAll: #(1 2))) not ].
    t check: 'not equal to an Array with same elements' that: [ ((ByteArray withAll: #(1 2 3)) = #(1 2 3)) not ].
    t check: 'equal ByteArrays share hash' that: [
        (ByteArray withAll: #(1 2 3)) hash = (ByteArray withAll: #(1 2 3)) hash ].

    "---- conversion and enumeration ----"
    t check: 'asArray produces an Array' equals: Array is: [ ba asArray class ].
    t check: 'asArray keeps elements' equals: #(255 0 128) is: [ ba asArray ].
    t check: 'do: visits every byte' equals: 383 is: [ | s | s := 0. ba do: [ :x | s := s + x ]. s ].
    t check: 'sum of bytes' equals: 383 is: [ ba sum ].
    t check: 'max of bytes' equals: 255 is: [ ba max ].
    t check: 'min of bytes' equals: 0 is: [ ba min ].
    t check: 'inject:into: over bytes' equals: 6 is: [ (ByteArray withAll: #(1 2 3)) inject: 0 into: [ :s :x | s + x ] ].
    t check: 'keysAndValuesDo: over bytes' equals: 6 is: [
        | s | s := 0. (ByteArray withAll: #(10 20 30)) keysAndValuesDo: [ :k :v | s := s + k ]. s ].
    t check: 'includes: present' that: [ ba includes: 128 ].
    t check: 'includes: absent' that: [ (ba includes: 200) not ].
    t check: 'occurrencesOf:' equals: 3 is: [ (ByteArray withAll: #(7 7 1 7)) occurrencesOf: 7 ].
    t check: 'indexOf: present' equals: 3 is: [ ba indexOf: 128 ].
    t check: 'indexOf: absent gives 0' equals: 0 is: [ ba indexOf: 200 ].
    t check: 'detect:ifNone: absent' equals: 0 is: [ ba detect: [ :x | x = 200 ] ifNone: [ 0 ] ].
    t check: 'select: over bytes' equals: #(255 128) is: [ (ba select: [ :x | x > 100 ]) asArray ].

    "---- copying and concatenation preserve the ByteArray species ----"
    t check: 'copy equals original' that: [ ba copy = ba ].
    t check: 'copy is independent' equals: 255 is: [
        | b c | b := ByteArray withAll: #(255 0 128). c := b copy. c at: 1 put: 9. b at: 1 ].
    t check: 'concatenation is a ByteArray' equals: ByteArray is: [
        ((ByteArray withAll: #(1 2)) , (ByteArray withAll: #(3 4))) class ].
    t check: 'concatenation joins bytes' equals: #(1 2 3 4) is: [
        ((ByteArray withAll: #(1 2)) , (ByteArray withAll: #(3 4))) asArray ].
    t check: 'copyFrom:to: is a ByteArray slice' equals: #(0 128) is: [ (ba copyFrom: 2 to: 3) asArray ].
    t check: 'copyWith: appends a byte' equals: #(1 2 3 9) is: [ ((ByteArray withAll: #(1 2 3)) copyWith: 9) asArray ].
    t check: 'reverse is a ByteArray' equals: ByteArray is: [ ba reverse class ].
    t check: 'reverse reverses bytes' equals: #(128 0 255) is: [ ba reverse asArray ].
    t check: 'collect: is a ByteArray' equals: ByteArray is: [ ((ByteArray withAll: #(1 2 3)) collect: [ :x | x * 2 ]) class ].
    t check: 'collect: maps bytes' equals: #(2 4 6) is: [ ((ByteArray withAll: #(1 2 3)) collect: [ :x | x * 2 ]) asArray ].
    t check: 'replaceFrom:to:with: bulk overwrite' equals: #(1 20 30 40 5) is: [
        | b | b := ByteArray withAll: #(1 2 3 4 5). b replaceFrom: 2 to: 4 with: (ByteArray withAll: #(20 30 40)). b asArray ].

    ^t report
]
