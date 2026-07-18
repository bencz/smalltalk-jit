"=== SequenceableProtocolTest: exercises the shared SequenceableCollection
 protocol on BOTH Array and OrderedCollection - indexing, searching, copying,
 slicing, reversing, concatenation, indexed enumeration, replaceFrom:, and the
 conversions. Documents a kernel bug: the ArrayedCollection copy / with:collect:
 family (which use 'species new: n') is broken for OrderedCollection.
 NOTE: everything is kept in a single driver block (one TestRun) so the exit
 code reflects every failure; the block is deliberately sized under the
 compiler's per-block limit. ==="

[
    | t a oc |
    t := TestRun named: 'SequenceableProtocol'.
    SPRun := t.
    a := #(10 20 30 40 50).
    oc := OrderedCollection withAll: #(10 20 30 40 50).

    "---- first / last ----"
    t check: 'Array first' equals: 10 is: [ a first ].
    t check: 'Array last' equals: 50 is: [ a last ].
    t check: 'OrderedCollection first' equals: 10 is: [ oc first ].

    "---- indexOf: family ----"
    t check: 'Array indexOf: present' equals: 3 is: [ a indexOf: 30 ].
    t check: 'Array indexOf: absent is 0' equals: 0 is: [ a indexOf: 99 ].
    t check: 'indexOf:startingAt:ifAbsent: skips earlier match' equals: 5 is: [
        #(10 20 30 40 20) indexOf: 20 startingAt: 3 ifAbsent: [ 0 ] ].
    t check: 'indexOf:startingAt:ifAbsent: honours absent block' equals: 99 is: [
        a indexOf: 999 startingAt: 1 ifAbsent: [ 99 ] ].
    t check: 'lastIndexOf: on Array' equals: 6 is: [ #(1 2 3 2 1 2) lastIndexOf: 2 ].
    "indexOf:ifAbsent: honours its absent block (previously discarded it for a hard 0)."
    t check: 'Array indexOf:ifAbsent: honors block' equals: -1 is: [ a indexOf: 999 ifAbsent: [ -1 ] ].
    t check: 'OC indexOf:ifAbsent: honors block' equals: -1 is: [ oc indexOf: 999 ifAbsent: [ -1 ] ].
    t check: 'indexOf: (no block) still answers 0 when absent' equals: 0 is: [ a indexOf: 999 ].

    "---- membership and searching ----"
    t check: 'Array includes: present' that: [ a includes: 30 ].
    t check: 'Array includes: absent' that: [ (a includes: 99) not ].
    t check: 'occurrencesOf: on Array' equals: 3 is: [ #(1 2 2 3 2) occurrencesOf: 2 ].
    t check: 'Array detect: first match' equals: 30 is: [ a detect: [ :x | x > 25 ] ].
    t check: 'Array detect:ifNone: none runs block' equals: -1 is: [ a detect: [ :x | x > 999 ] ifNone: [ -1 ] ].
    t expect: 'detect: with no match raises NotFoundError' raises: NotFoundError in: [ a detect: [ :x | x > 999 ] ].
    t check: 'startsWith: prefix' that: [ a startsWith: #(10 20) ].
    t check: 'startsWith: non-prefix' that: [ (a startsWith: #(20 30)) not ].

    "---- reducing / counting ----"
    t check: 'Array inject:into:' equals: 150 is: [ a inject: 0 into: [ :s :x | s + x ] ].
    t check: 'Array sum' equals: 150 is: [ a sum ].
    t check: 'Array average' equals: 30 is: [ a average ].
    t check: 'Array count:' equals: 2 is: [ a count: [ :x | x > 30 ] ].

    "---- slicing / copying (works on both species) ----"
    t check: 'Array copyFrom:to:' equals: #(20 30 40) is: [ a copyFrom: 2 to: 4 ].
    t check: 'Array copyFrom: to end' equals: #(30 40 50) is: [ a copyFrom: 3 ].
    t check: 'OC copyFrom:to:' equals: #(20 30 40) is: [ (oc copyFrom: 2 to: 4) asArray ].
    t check: 'Array copyUpTo:' equals: #(10 20) is: [ a copyUpTo: 30 ].
    t check: 'Array copyAfter:' equals: #(30 40 50) is: [ a copyAfter: 20 ].

    "---- reversing ----"
    t check: 'Array reverse' equals: #(50 40 30 20 10) is: [ a reverse ].
    t check: 'Array reversed' equals: #(50 40 30 20 10) is: [ a reversed ].
    t check: 'reverse yields a new object' that: [ (a reverse == a) not ].
    t check: 'OC reverse' equals: #(50 40 30 20 10) is: [ oc reverse asArray ].

    "---- concatenation ----"
    t check: 'Array , Array yields an Array' equals: Array is: [ (a , #(60)) class ].
    t check: 'Array , Array joins' equals: #(10 20 30 40 50 60 70) is: [ a , #(60 70) ].
    t check: 'OC , Array joins' equals: #(10 20 30 40 50 60) is: [ (oc , #(60)) asArray ].

    "---- indexed enumeration ----"
    t check: 'keysAndValuesDo: sums keys' equals: 15 is: [
        | s | s := 0. a keysAndValuesDo: [ :k :v | s := s + k ]. s ].
    t check: 'keysAndValuesDo: sums values' equals: 150 is: [
        | s | s := 0. a keysAndValuesDo: [ :k :v | s := s + v ]. s ].
    t check: 'do:separatedBy: interleaves separator' equals: '10, 20, 30, 40, 50' is: [
        | s | s := CollectionStream with: (String new: 0).
        a do: [ :x | s nextPutAll: x printString ] separatedBy: [ s nextPutAll: ', ' ].
        s contents ].

    "---- with:collect: and copy family (ArrayedCollection-shaped): Array only ----"
    t check: 'Array with:collect: pairwise' equals: #(11 22 33) is: [
        #(1 2 3) with: #(10 20 30) collect: [ :x :y | x + y ] ].
    t check: 'Array copyWith: appends' equals: #(10 20 30 40 50 60) is: [ a copyWith: 60 ].
    t check: 'Array copyReplaceFrom:to:with:' equals: #(10 99 98 40 50) is: [
        a copyReplaceFrom: 2 to: 3 with: #(99 98) ].
    "BUG: these inherited ArrayedCollection methods break on OrderedCollection
     because 'species new: n' builds a logically-empty OC then indexes it."
    t expect: 'BUG: OC with:collect: raises' raises: OutOfRangeError in: [ oc with: oc collect: [ :x :y | x + y ] ].
    t expect: 'BUG: OC copyWith: raises' raises: OutOfRangeError in: [ oc copyWith: 60 ].
    t expect: 'BUG: OC copyReplaceFrom:to:with: raises' raises: OutOfRangeError in: [ oc copyReplaceFrom: 2 to: 3 with: #(99) ].

    "---- replaceFrom: mutates in place (both species) ----"
    t check: 'Array replaceFrom:to:with:' equals: #(1 20 30 40 5) is: [
        | m | m := #(1 2 3 4 5) copy. m replaceFrom: 2 to: 4 with: #(20 30 40). m ].
    t check: 'Array replaceFrom:to:with:startingAt:' equals: #(1 98 97 4 5) is: [
        | m | m := #(1 2 3 4 5) copy. m replaceFrom: 2 to: 3 with: #(99 98 97) startingAt: 2. m ].
    t check: 'OC replaceFrom:to:with:' equals: #(1 20 30 40 5) is: [
        | m | m := OrderedCollection withAll: #(1 2 3 4 5). m replaceFrom: 2 to: 4 with: #(20 30 40). m asArray ].

    "---- conversions: identity vs equality and round-trips ----"
    t check: 'Array asArray equal but not identical' that: [ (a asArray = a) and: [ (a asArray == a) not ] ].
    t check: 'Array asOrderedCollection round-trips' equals: #(10 20 30 40 50) is: [ a asOrderedCollection asArray ].
    t check: 'OC asArray round-trips' equals: #(10 20 30 40 50) is: [ oc asArray ].
    t check: 'OC asArray yields an Array' equals: Array is: [ oc asArray class ].
    t check: 'Array asBag preserves count' equals: 5 is: [ a asBag size ].
    t check: 'asSet dedups' equals: 3 is: [ #(1 1 2 2 3) asSet size ].
    t check: 'asSortedCollection sorts' equals: #(1 2 3) is: [ #(3 1 2) asSortedCollection asArray ].
    t check: 'asBag then asSet collapses duplicates' equals: 2 is: [ #(5 5 9) asBag asSet size ].
]

"=== Second driver block: arrays that hold BOTH negative and positive numbers,
 written as #() literals (the parser accepts a leading - inside #( ... )). This
 block SHARES the SequenceableProtocol TestRun through the SPRun global set by
 the first block, and only this final block runs ^t report, so every failure
 from both blocks is counted in the exit code (no false pass). ==="
[
    | t a b oc |
    t := SPRun.
    a := #(-3 5 -1 2).
    b := #(7 -8 0 -8).
    oc := OrderedCollection withAll: a.

    "---- the negative-in-#() literal itself parses to the right values ----"
    t check: 'negative literal parses' equals: -3 is: [ #(-3 5 -1 2) first ].
    t check: 'negative literal size' equals: 4 is: [ #(-3 5 -1 2) size ].
    t check: 'negative float literal' equals: -2.5 is: [ #(1 -2.5 3) at: 2 ] .
    t check: 'nested negative literal' equals: -1 is: [ (#(#(-1) 2) at: 1) first ].

    "---- indexing / access preserve sign ----"
    t check: 'signed first' equals: -3 is: [ a first ].
    t check: 'signed last' equals: 2 is: [ a last ].
    t check: 'signed at: negative element' equals: -1 is: [ a at: 3 ].

    "---- indexOf: / membership on negatives and positives ----"
    t check: 'signed indexOf: negative present' equals: 1 is: [ a indexOf: -3 ].
    t check: 'signed indexOf: positive present' equals: 2 is: [ a indexOf: 5 ].
    t check: 'signed indexOf: absent negative' equals: 0 is: [ a indexOf: -99 ].
    t check: 'signed includes: negative' that: [ a includes: -1 ].
    t check: 'signed includes: absent negative' that: [ (a includes: -99) not ].
    t check: 'signed occurrencesOf: repeated negative' equals: 2 is: [ b occurrencesOf: -8 ].

    "---- reductions across mixed signs ----"
    t check: 'signed sum via inject' equals: 3 is: [ a inject: 0 into: [ :s :x | s + x ] ].
    t check: 'signed sum' equals: 3 is: [ a sum ].
    t check: 'signed max fold' equals: 5 is: [ a inject: a first into: [ :m :x | m max: x ] ].
    t check: 'signed min fold' equals: -3 is: [ a inject: a first into: [ :m :x | m min: x ] ].
    t check: 'signed count: negatives' equals: 2 is: [ a count: [ :x | x < 0 ] ].

    "---- detect: first negative / first positive ----"
    t check: 'signed detect: first negative' equals: -3 is: [ a detect: [ :x | x < 0 ] ].
    t check: 'signed detect: first positive' equals: 5 is: [ a detect: [ :x | x > 0 ] ].

    "---- select: / reject: by sign (both answer an OrderedCollection here) ----"
    t check: 'signed select: negatives' equals: #(-3 -1) is: [ (a select: [ :x | x < 0 ]) asArray ].
    t check: 'signed reject: negatives' equals: #(5 2) is: [ (a reject: [ :x | x < 0 ]) asArray ].

    "---- collect: negated flips every sign, preserving order and Array species ----"
    t check: 'signed collect: negated' equals: #(3 -5 1 -2) is: [ a collect: [ :x | x negated ] ].

    "---- reverse and slicing preserve signed order ----"
    t check: 'signed reverse' equals: #(2 -1 5 -3) is: [ a reverse ].
    t check: 'signed copyFrom:2 to:3' equals: #(5 -1) is: [ a copyFrom: 2 to: 3 ].

    "---- concatenation of two mixed-sign arrays (checked by shape) ----"
    t check: 'signed , size' equals: 8 is: [ (a , b) size ].
    t check: 'signed , first' equals: -3 is: [ (a , b) first ].
    t check: 'signed , last' equals: -8 is: [ (a , b) last ].
    t check: 'signed , crosses the boundary' equals: 7 is: [ (a , b) at: 5 ].

    "---- sorting orders negatives before positives ----"
    t check: 'signed asSortedCollection ascending' equals: #(-3 -1 2 5) is: [ a asSortedCollection asArray ].

    "---- same protocol on OrderedCollection with signed values ----"
    t check: 'OC signed sum' equals: 3 is: [ oc inject: 0 into: [ :s :x | s + x ] ].
    t check: 'OC signed indexOf: negative' equals: 3 is: [ oc indexOf: -1 ].
    t check: 'OC signed reverse' equals: #(2 -1 5 -3) is: [ oc reverse asArray ].

    ^t report
]
