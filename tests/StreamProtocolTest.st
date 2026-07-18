"StreamProtocolTest: exercises the generic Stream reading/writing protocol
 (next, next:, peek, atEnd, upToEnd, upTo:, skip:, do:, contents, reset,
 nextMatchFor:, nextPut:, nextPutAll:, next:put:, cr/lf/space/tab/tab:, show:,
 showCr:) plus the Iterator enumerating protocol layered on top of a Stream
 (inject:into:, detect:ifNone:, detect:, anySatisfy:). Read streams are built
 with `CollectionStream on:`; write streams with `CollectionStream with:`.
 Behaviours confirmed against smalltalk/Streams/Stream.st, CollectionStream.st
 and Iterator.st. Positioning specifics live in PositionableStreamTest.st."
[
    | t s w oc arr got exp |
    t := TestRun named: 'StreamProtocol'.

    "--- reading: next / peek / atEnd ---"
    t check: 'next yields elements in order' that: [
        s := CollectionStream on: 'hello'.
        (s next = $h) and: [ (s next = $e) and: [ s next = $l ] ] ].
    t check: 'peek does not advance the position' that: [
        s := CollectionStream on: 'hello'.
        (s peek = $h) and: [ (s peek = $h) and: [ s position = 1 ] ] ].
    t check: 'next advances past a prior peek' that: [
        s := CollectionStream on: 'hi'.
        (s peek = $h) and: [ (s next = $h) and: [ s peek = $i ] ] ].
    t check: 'atEnd is false on a fresh non-empty read stream' that: [
        s := CollectionStream on: 'x'.
        s atEnd not ].
    t check: 'atEnd is true once every element is consumed' that: [
        s := CollectionStream on: 'ab'.
        s next. s next.
        s atEnd ].
    t check: 'next past the end returns nil' equals: nil is: [
        s := CollectionStream on: 'a'.
        s next. s next ].
    t check: 'peek past the end returns nil' equals: nil is: [
        s := CollectionStream on: 'a'.
        s next. s peek ].

    "--- reading: next: n and boundaries ---"
    t check: 'next: n returns the next n elements' equals: 'abc' is: [
        s := CollectionStream on: 'abcdef'.
        s next: 3 ].
    t check: 'next: 0 returns an empty collection' equals: '' is: [
        s := CollectionStream on: 'abc'.
        s next: 0 ].
    t check: 'next: n up to the exact end does not raise' equals: 'abc' is: [
        s := CollectionStream on: 'abc'.
        s next: 3 ].
    t expect: 'next: beyond the remaining elements raises OutOfRangeError'
        raises: OutOfRangeError in: [
            s := CollectionStream on: 'abc'.
            s next: 5 ].

    "--- reading: upToEnd / upTo: ---"
    t check: 'upToEnd from the start returns the whole collection' equals: 'abcdef' is: [
        s := CollectionStream on: 'abcdef'.
        s upToEnd ].
    t check: 'upToEnd after partial read returns the remainder' equals: 'def' is: [
        s := CollectionStream on: 'abcdef'.
        s next: 3.
        s upToEnd ].
    t check: 'upTo: returns the prefix before the delimiter' equals: 'abc' is: [
        s := CollectionStream on: 'abc,def'.
        s upTo: $, ].
    t check: 'upTo: consumes the delimiter so next reads past it' that: [
        s := CollectionStream on: 'abc,def'.
        s upTo: $,.
        s next = $d ].
    t check: 'upTo: with an absent delimiter returns the rest and is atEnd' that: [
        s := CollectionStream on: 'abc'.
        got := s upTo: $z.
        (got = 'abc') and: [ s atEnd ] ].

    "--- reading: skip: / do: / contents / reset ---"
    t check: 'skip: advances the read position' that: [
        s := CollectionStream on: 'abcdef'.
        s skip: 2.
        s next = $c ].
    t check: 'do: visits every remaining element in order' that: [
        s := CollectionStream on: 'abc'.
        oc := OrderedCollection new.
        s do: [:c | oc add: c].
        oc asArray = (Array with: $a with: $b with: $c) ].
    t check: 'contents returns the whole collection regardless of position' equals: 'abcdef' is: [
        s := CollectionStream on: 'abcdef'.
        s next: 4.
        s contents ].
    t check: 'reset rewinds so next re-reads the first element' that: [
        s := CollectionStream on: 'abc'.
        s next: 3.
        s reset.
        s next = $a ].

    "--- reading: nextMatchFor: (always consumes one element) ---"
    t check: 'nextMatchFor: returns true on a match' that: [
        s := CollectionStream on: 'abc'.
        s nextMatchFor: $a ].
    t check: 'nextMatchFor: advances even on a match' that: [
        s := CollectionStream on: 'abc'.
        s nextMatchFor: $a.
        s position = 2 ].
    t check: 'nextMatchFor: returns false on a mismatch' that: [
        s := CollectionStream on: 'abc'.
        (s nextMatchFor: $z) = false ].

    "--- writing: nextPut: / nextPutAll: / next:put: ---"
    t check: 'nextPut: then contents yields the single element' equals: 'a' is: [
        w := CollectionStream with: (String new: 0).
        w nextPut: $a.
        w contents ].
    t check: 'nextPutAll: appends a whole collection' equals: 'hello' is: [
        w := CollectionStream with: (String new: 0).
        w nextPutAll: 'hello'.
        w contents ].
    t check: 'successive nextPut: accumulate in order' equals: 'abc' is: [
        w := CollectionStream with: (String new: 0).
        w nextPut: $a; nextPut: $b; nextPut: $c.
        w contents ].
    t check: 'next: n put: repeats the element n times' equals: 'xxx' is: [
        w := CollectionStream with: (String new: 0).
        w next: 3 put: $x.
        w contents ].

    "--- writing: whitespace helpers cr / lf / space / tab / tab: ---"
    t check: 'space writes a single space character' that: [
        w := CollectionStream with: (String new: 0).
        w space.
        w contents = (String with: Character space) ].
    t check: 'tab writes a single tab character' that: [
        w := CollectionStream with: (String new: 0).
        w tab.
        w contents = (String with: Character tab) ].
    t check: 'cr writes a single carriage-return character' that: [
        w := CollectionStream with: (String new: 0).
        w cr.
        w contents = (String with: Character cr) ].
    t check: 'lf writes a single line-feed character' that: [
        w := CollectionStream with: (String new: 0).
        w lf.
        w contents = (String with: Character lf) ].
    t check: 'tab: n writes n tab characters' equals: 3 is: [
        w := CollectionStream with: (String new: 0).
        w tab: 3.
        w contents size ].

    "--- writing: show: / showCr: ---"
    t check: 'show: appends its argument and returns it' that: [
        w := CollectionStream with: (String new: 0).
        got := w show: 'hi'.
        (w contents = 'hi') and: [ got = 'hi' ] ].
    t check: 'showCr: appends the string then a line feed' that: [
        w := CollectionStream with: (String new: 0).
        w showCr: 'hi'.
        w contents = ('hi', (String with: Character lf)) ].

    "--- writing: build a non-trivial string exactly ---"
    exp := 'ab', (String with: Character tab), 'cd',
           (String with: Character space),
           (String with: Character cr),
           (String with: Character lf).
    t check: 'a mixed cascade builds the exact expected string' equals: exp is: [
        w := CollectionStream with: (String new: 0).
        w nextPutAll: 'ab'; tab; nextPutAll: 'cd'; space; cr; lf.
        w contents ].

    "--- round trip: write then read back ---"
    t check: 'write stream contents re-read by a read stream are equal' that: [
        w := CollectionStream with: (String new: 0).
        w nextPutAll: 'roundtrip'.
        s := CollectionStream on: w contents.
        s upToEnd = 'roundtrip' ].

    "--- Iterator enumerating protocol over a Stream ---"
    t check: 'inject:into: folds every streamed element' equals: 14 is: [
        arr := CollectionStream on: #(3 1 4 1 5).
        arr inject: 0 into: [:a :b | a + b] ].
    t check: 'detect:ifNone: returns the first satisfying element' equals: 5 is: [
        arr := CollectionStream on: #(3 1 4 1 5).
        arr detect: [:x | x > 4] ifNone: [ 0 ] ].
    t check: 'detect:ifNone: runs the none block when nothing matches' equals: 0 is: [
        arr := CollectionStream on: #(3 1 4 1 5).
        arr detect: [:x | x > 100] ifNone: [ 0 ] ].
    t expect: 'detect: with no match raises NotFoundError'
        raises: NotFoundError in: [
            arr := CollectionStream on: #(1 2 3).
            arr detect: [:x | x > 100] ].
    t check: 'anySatisfy: is true when an element matches' that: [
        arr := CollectionStream on: #(3 1 4 1 5).
        arr anySatisfy: [:x | x = 4] ].
    t check: 'anySatisfy: is false when nothing matches' that: [
        arr := CollectionStream on: #(3 1 4 1 5).
        (arr anySatisfy: [:x | x = 99]) = false ].

    ^t report
]
