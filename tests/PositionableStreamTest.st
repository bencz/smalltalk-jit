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

"PositionableStreamTest: exercises the positioning protocol of
 PositionableStream / CollectionStream: position, position:, reset, setToEnd,
 skip: (including negative offsets), peek, peekFor:, atEnd, isEmpty, plus the
 upTo: / upToAll: delimiter scans and their effect on the read position. Also
 covers position boundary rules (valid range 1..end, out-of-range raises Error)
 and a position:-based write/re-read round trip. Behaviours confirmed against
 smalltalk/Streams/PositionableStream.st and CollectionStream.st. The generic
 read/write protocol is covered separately in StreamProtocolTest.st.
 NOTE: CollectionStream>>isEmpty is defined as `position = 1`, so a fresh read
 stream reports isEmpty=true even though it has content; these checks assert
 that actual (position-at-start) semantics."
[
    | t s w got |
    t := TestRun named: 'PositionableStream'.

    "--- position accessor ---"
    t check: 'position starts at 1' equals: 1 is: [
        s := CollectionStream on: 'abc'.
        s position ].
    t check: 'position advances by one after next' equals: 2 is: [
        s := CollectionStream on: 'abc'.
        s next.
        s position ].
    t check: 'position advances by n after next: n' equals: 4 is: [
        s := CollectionStream on: 'abcdef'.
        s next: 3.
        s position ].

    "--- position: setter and re-reading ---"
    t check: 'position: sets the read point' that: [
        s := CollectionStream on: 'abcdef'.
        s position: 3.
        s next = $c ].
    t check: 'position: back to an earlier point re-reads it' that: [
        s := CollectionStream on: 'abcdef'.
        s next: 4.
        s position: 2.
        s next = $b ].

    "--- reset / setToEnd ---"
    t check: 'reset returns to the start' that: [
        s := CollectionStream on: 'abc'.
        s next: 2.
        s reset.
        (s position = 1) and: [ s next = $a ] ].
    t check: 'setToEnd makes atEnd true' that: [
        s := CollectionStream on: 'abc'.
        s setToEnd.
        s atEnd ].
    t check: 'setToEnd leaves position at size + 1' that: [
        s := CollectionStream on: 'abcde'.
        s setToEnd.
        s position = (s size + 1) ].

    "--- skip: forwards and backwards ---"
    t check: 'skip: advances by a positive offset' that: [
        s := CollectionStream on: 'abcdef'.
        s skip: 3.
        s next = $d ].
    t check: 'skip: with a negative offset moves back' that: [
        s := CollectionStream on: 'abcdef'.
        s next: 4.
        s skip: -2.
        s next = $c ].
    t check: 'skip: to the end makes atEnd true' that: [
        s := CollectionStream on: 'abc'.
        s skip: 3.
        s atEnd ].

    "--- peek / peekFor: ---"
    t check: 'peek returns the current element without advancing' that: [
        s := CollectionStream on: 'abc'.
        s next.
        (s peek = $b) and: [ s position = 2 ] ].
    t check: 'peekFor: consumes and returns true on a match' that: [
        s := CollectionStream on: 'abc'.
        (s peekFor: $a) and: [ s position = 2 ] ].
    t check: 'peekFor: leaves position unchanged and returns false on a mismatch' that: [
        s := CollectionStream on: 'abc'.
        ((s peekFor: $z) = false) and: [ s position = 1 ] ].

    "--- atEnd mid stream ---"
    t check: 'atEnd is false mid stream' that: [
        s := CollectionStream on: 'abc'.
        s next.
        s atEnd not ].
    t check: 'atEnd is true after reading everything with next: n' that: [
        s := CollectionStream on: 'abc'.
        s next: 3.
        s atEnd ].

    "--- upTo: / upToAll: scanning and positioning ---"
    t check: 'successive upTo: split the stream on the delimiter' that: [
        s := CollectionStream on: 'aa,bb,cc'.
        (((s upTo: $,) = 'aa') and: [ (s upTo: $,) = 'bb' ])
            and: [ s upToEnd = 'cc' ] ].
    t check: 'upToAll: returns content before the match and consumes it' that: [
        s := CollectionStream on: 'abcXYdef'.
        ((s upToAll: 'XY') = 'abc') and: [ s next = $d ] ].
    t check: 'upToAll: with no match returns the whole collection and is atEnd' that: [
        s := CollectionStream on: 'hello'.
        ((s upToAll: 'ZZ') = 'hello') and: [ s atEnd ] ].

    "--- isEmpty (position = 1 semantics) ---"
    t check: 'fresh read stream reports isEmpty true (position at start)' that: [
        s := CollectionStream on: 'abc'.
        s isEmpty ].
    t check: 'read stream reports isEmpty false once advanced' that: [
        s := CollectionStream on: 'abc'.
        s next.
        s isEmpty = false ].
    t check: 'fresh write stream reports isEmpty true' that: [
        w := CollectionStream with: (String new: 0).
        w isEmpty ].
    t check: 'write stream reports isEmpty false after nextPut:' that: [
        w := CollectionStream with: (String new: 0).
        w nextPut: $a.
        w isEmpty = false ].

    "--- position boundary rules (valid range 1..end) ---"
    t check: 'position: 1 is the valid minimum' that: [
        s := CollectionStream on: 'abc'.
        s position: 1.
        s position = 1 ].
    t check: 'position: to end (size + 1) is valid and atEnd' that: [
        s := CollectionStream on: 'abc'.
        s position: 4.
        s atEnd ].
    t expect: 'position: 0 raises Error (below minimum)'
        raises: Error in: [
            s := CollectionStream on: 'abc'.
            s position: 0 ].
    t expect: 'position: past end raises Error (above maximum)'
        raises: Error in: [
            s := CollectionStream on: 'abc'.
            s position: 5 ].

    "--- round trip via position: write then re-read the same stream ---"
    t check: 'position: rewinds a write stream so its bytes can be read back' that: [
        w := CollectionStream with: (String new: 0).
        w nextPutAll: 'wxyz'.
        w position: 1.
        got := w next.
        (got = $w) and: [ ((w next: 3) = 'xyz') and: [ w atEnd ] ] ].

    ^t report
]
