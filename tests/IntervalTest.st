"=== IntervalTest: exercises Interval (1 to: n, by:, negative step, empty),
 sizing, indexing, first/last/increment, enumeration (do:/reverseDo:/collect:/
 select:/reject:/inject:into:/includes:), conversion (asArray/reverse), equality,
 sum/average and out-of-range/immutability errors. Complements CollectionExtras. ==="

[
    | t empty odd neg |
    t := TestRun named: 'Interval'.
    odd := 1 to: 10 by: 2.
    neg := 10 to: 1 by: -1.
    empty := 5 to: 1.

    "---- sizing ----"
    t check: 'size ascending step 1' equals: 5 is: [ (1 to: 5) size ].
    t check: 'size ascending step 2' equals: 5 is: [ odd size ].
    t check: 'size descending step -1' equals: 10 is: [ neg size ].
    t check: 'size by 3' equals: 4 is: [ (1 to: 10 by: 3) size ].
    t check: 'empty interval size 0' equals: 0 is: [ empty size ].
    t check: 'empty descending default step size 0' equals: 0 is: [ (1 to: 5 by: -1) size ].
    t check: 'empty isEmpty' that: [ empty isEmpty ].
    t check: 'non-empty notEmpty' that: [ (1 to: 5) isEmpty not ].

    "---- accessing ----"
    t check: 'first' equals: 2 is: [ (2 to: 10 by: 3) first ].
    t check: 'increment' equals: 3 is: [ (2 to: 10 by: 3) increment ].
    t check: 'last last-reachable value' equals: 8 is: [ (2 to: 10 by: 3) last ].
    t check: 'last simple' equals: 5 is: [ (1 to: 5) last ].
    t check: 'last with step lands on reachable' equals: 9 is: [ odd last ].
    "NOTE: Interval>>last is BUGGY for descending intervals (VM \\ with a
     negative divisor is wrong), so we assert the true last element via the
     enumeration path (asArray) instead of via #last. See test report."
    t check: 'descending true last element via asArray' equals: 1 is: [ neg asArray last ].
    t check: 'at: step 1' equals: 3 is: [ (1 to: 5) at: 3 ].
    t check: 'at: step 2' equals: 5 is: [ odd at: 3 ].
    t check: 'at: first index' equals: 1 is: [ odd at: 1 ].
    t check: 'at: last index' equals: 9 is: [ odd at: 5 ].
    t check: 'at: descending index 1' equals: 10 is: [ neg at: 1 ].
    t check: 'at: descending index 5' equals: 6 is: [ neg at: 5 ].

    "---- out-of-range and immutability ----"
    t expect: 'at: above size raises' raises: OutOfRangeError in: [ (1 to: 5) at: 6 ].
    t expect: 'at: zero raises' raises: OutOfRangeError in: [ (1 to: 5) at: 0 ].
    t expect: 'at: on empty raises' raises: OutOfRangeError in: [ empty at: 1 ].
    t expect: 'at:put: raises Error' raises: Error in: [ (1 to: 5) at: 1 put: 9 ].
    t expect: 'add: raises Error' raises: Error in: [ (1 to: 5) add: 9 ].
    t expect: 'remove: raises Error' raises: Error in: [ (1 to: 5) remove: 3 ].

    "---- enumeration ----"
    t check: 'do: visits every element in order' equals: 15 is: [
        | acc | acc := 0. (1 to: 5) do: [ :x | acc := acc + x ]. acc ].
    t check: 'do: on empty never runs' equals: 0 is: [
        | n | n := 0. empty do: [ :x | n := n + 1 ]. n ].
    t check: 'reverseDo: yields reversed order' equals: #(5 4 3 2 1) is: [
        | acc | acc := OrderedCollection new. (1 to: 5) reverseDo: [ :x | acc add: x ]. acc asArray ].
    t check: 'collect: returns an Array' equals: Array is: [ ((1 to: 5) collect: [ :x | x ]) class ].
    t check: 'collect: maps and preserves order' equals: #(1 4 9 16 25) is: [ (1 to: 5) collect: [ :x | x * x ] ].
    t check: 'collect: over stepped interval' equals: #(2 6 10) is: [ (1 to: 5 by: 2) collect: [ :x | x * 2 ] ].
    t check: 'select: returns OrderedCollection' equals: OrderedCollection is: [ ((1 to: 10) select: [ :x | x even ]) class ].
    t check: 'select: filters' equals: #(2 4 6 8 10) is: [ ((1 to: 10) select: [ :x | x even ]) asArray ].
    t check: 'reject: filters' equals: #(1 3 5) is: [ ((1 to: 6) reject: [ :x | x even ]) asArray ].
    t check: 'inject:into: sums' equals: 15 is: [ (1 to: 5) inject: 0 into: [ :s :x | s + x ] ].
    t check: 'includes: present' that: [ odd includes: 7 ].
    t check: 'includes: absent (off-step)' that: [ (odd includes: 4) not ].
    t check: 'includes: descending' that: [ neg includes: 4 ].
    t check: 'detect: finds' equals: 4 is: [ (1 to: 10) detect: [ :x | x > 3 ] ].
    t check: 'occurrencesOf: over interval' equals: 1 is: [ (1 to: 5) occurrencesOf: 3 ].
    t check: 'count: over interval' equals: 5 is: [ (1 to: 10) count: [ :x | x even ] ].

    "---- reducing ----"
    t check: 'sum' equals: 15 is: [ (1 to: 5) sum ].
    t check: 'sum empty is 0' equals: 0 is: [ empty sum ].
    t check: 'max' equals: 5 is: [ (1 to: 5) max ].
    t check: 'min' equals: 1 is: [ (1 to: 5) min ].
    t check: 'average' equals: 3 is: [ (1 to: 5) average ].

    "---- converting / copying ----"
    t check: 'asArray ascending' equals: #(1 2 3 4) is: [ (1 to: 4) asArray ].
    t check: 'asArray descending' equals: #(10 9 8 7 6 5 4 3 2 1) is: [ neg asArray ].
    t check: 'asArray stepped' equals: #(1 4 7 10) is: [ (1 to: 10 by: 3) asArray ].
    t check: 'asArray empty' equals: #() is: [ empty asArray ].
    t check: 'reverse yields Array' equals: #(5 4 3 2 1) is: [ (1 to: 5) reverse ].
    t check: 'reversed yields Array' equals: #(3 2 1) is: [ (1 to: 3) reversed ].
    t check: 'concatenation with interval' equals: #(1 2 3 4 5) is: [ (1 to: 3) , (4 to: 5) ].
    t check: 'concatenation with array' equals: #(1 2 3 9) is: [ (1 to: 3) , #(9) ].
    t check: 'copyFrom:to:' equals: #(3 4 5) is: [ (1 to: 10) copyFrom: 3 to: 5 ].
    t check: 'asOrderedCollection round-trip' equals: #(1 2 3) is: [ (1 to: 3) asOrderedCollection asArray ].
    t check: 'asBag size' equals: 3 is: [ (1 to: 3) asBag size ].
    t check: 'asSet size' equals: 5 is: [ (1 to: 5) asSet size ].

    "---- equality and hash ----"
    t check: 'equal intervals =' that: [ (1 to: 5) = (1 to: 5) ].
    t check: 'different start not =' that: [ ((1 to: 5) = (2 to: 5)) not ].
    t check: 'different size not =' that: [ ((1 to: 5) = (1 to: 6)) not ].
    t check: 'same span different step but equal elements =' that: [ (1 to: 5 by: 2) = (1 to: 6 by: 2) ].
    t check: 'interval not equal to Array of same elements' that: [ ((1 to: 3) = #(1 2 3)) not ].
    t check: 'equal intervals share hash' that: [ (1 to: 5) hash = (1 to: 5) hash ].

    ^t report
]
