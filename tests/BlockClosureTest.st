"Helper classes for the closure tests below.
 Counter>>makeAdder: returns a closure over the method parameter n; each call
 gets a fresh binding. makeCountUp returns a closure over a private temp so two
 counters are independent. CaptureHelper exercises closures over an instance
 variable (captured through the receiver, so they see the CURRENT ivar value)."

Counter := Object [
    makeAdder: n [ ^[:x | x + n] ]
    makeCountUp [ | c | c := 0. ^[ c := c + 1. c ] ]
]

CaptureHelper := Object [
    | v |
    setV: x [ v := x. ^self ]
    reader [ ^[ v ] ]
    writer [ ^[:x | v := x] ]
    v [ ^v ]
]

[
    | t |
    t := TestRun named: 'BlockClosure'.

    "--- basic value forms and the returned value (arities 0..3) ---"
    t check: 'empty block value is nil' that: [ [ ] value isNil ].
    t check: 'value yields last expression' equals: 42 is: [ [ 40 + 2 ] value ].
    t check: 'value: passes one arg' equals: 11 is: [ [:a | a + 1] value: 10 ].
    t check: 'value:value: passes two args' equals: 30 is: [ [:a :b | a + b] value: 10 value: 20 ].
    t check: 'value:value:value: passes three args' equals: 6 is: [ [:a :b :c | a + b + c] value: 1 value: 2 value: 3 ].
    t check: 'block value uses last statement only' equals: 3 is: [ [ 1. 2. 3 ] value ].

    "--- valueWithArguments: for arities 0..4 (the 4-arg block has no
         value:value:value:value: selector in this kernel, so vwa is the way) ---"
    t check: 'vwa with 0 args' equals: 7 is: [ [ 7 ] valueWithArguments: #() ].
    t check: 'vwa with 1 arg' equals: 5 is: [ [:a | a] valueWithArguments: (Array with: 5) ].
    t check: 'vwa with 2 args' equals: 12 is: [ [:a :b | a + b] valueWithArguments: (Array with: 5 with: 7) ].
    t check: 'vwa with 3 args' equals: 15 is: [ [:a :b :c | a + b + c] valueWithArguments: (Array with: 4 with: 5 with: 6) ].
    t check: 'vwa with 4 args' equals: 10 is: [ [:a :b :c :d | a + b + c + d] valueWithArguments: (Array with: 1 with: 2 with: 3 with: 4) ].

    "--- calling with the WRONG argument count raises OutOfRangeError ---"
    t expect: '0-arg block sent value: raises' raises: OutOfRangeError in: [ | b | b := [ 1 ]. b value: 9 ].
    t expect: '1-arg block sent value (too few) raises' raises: OutOfRangeError in: [ | b | b := [:a | a]. b value ].
    t expect: '2-arg block sent value: (too few) raises' raises: OutOfRangeError in: [ | b | b := [:a :c | a]. b value: 1 ].
    t expect: '1-arg block sent value:value: (too many) raises' raises: OutOfRangeError in: [ | b | b := [:a | a]. b value: 1 value: 2 ].
    t expect: 'vwa with too few args raises' raises: OutOfRangeError in: [ | b | b := [:a | a]. b valueWithArguments: #() ].
    t expect: 'vwa with too many args raises' raises: OutOfRangeError in: [ | b | b := [:a | a]. b valueWithArguments: (Array with: 1 with: 2) ].

    "--- closures read the CURRENT value of a captured outer temp ---"
    t check: 'closure reads latest outer value' equals: 99 is: [
        | x blk | x := 1. blk := [ x ]. x := 99. blk value ].
    t check: 'closure re-reads outer temp on each call' that: [
        | x blk a b | x := 3. blk := [ x * 10 ].
        a := blk value. x := 4. b := blk value.
        (a = 30) and: [ b = 40 ] ].

    "--- mutating a captured outer temp is visible after the block runs ---"
    t check: 'closure mutation of outer temp persists' equals: 20 is: [
        | sum blk | sum := 0. blk := [ sum := sum + 10 ].
        blk value. blk value. sum ].
    t check: 'two closures share one captured temp' that: [
        | n inc get | n := 0.
        inc := [ n := n + 1 ]. get := [ n ].
        inc value. inc value. inc value.
        get value = 3 ].

    "--- a method returning a block that closes over a parameter/counter ---"
    t check: 'makeAdder captures its parameter' equals: 105 is: [
        (Counter new makeAdder: 5) value: 100 ].
    t check: 'two adders over different captures are independent' that: [
        | c add5 add10 | c := Counter new.
        add5 := c makeAdder: 5. add10 := c makeAdder: 10.
        ((add5 value: 100) = 105) and: [ (add10 value: 100) = 110 ] ].
    t check: 'each makeCountUp closure has a private counter' that: [
        | c up1 up2 | c := Counter new.
        up1 := c makeCountUp. up2 := c makeCountUp.
        up1 value. up1 value.
        (up1 value = 3) and: [ up2 value = 1 ] ].

    "--- closures over an instance variable see the current ivar and can mutate it ---"
    t check: 'closure reads current ivar value' equals: 20 is: [
        | h r | h := CaptureHelper new setV: 10. r := h reader. h setV: 20. r value ].
    t check: 'closure mutates ivar through the receiver' equals: 99 is: [
        | h w | h := CaptureHelper new setV: 0. w := h writer. w value: 99. h v ].

    "--- nested closures: a block that returns a block closing over the outer temp ---"
    t check: 'nested closure captures the outer binding' equals: 7 is: [
        | outer | outer := [:base | [:d | base + d] ].
        (outer value: 3) value: 4 ].
    t check: 'nested closures from same maker are independent' that: [
        | outer f3 f9 | outer := [:base | [:d | base + d] ].
        f3 := outer value: 3. f9 := outer value: 9.
        ((f3 value: 100) = 103) and: [ (f9 value: 100) = 109 ] ].

    "--- recursion via a block bound to a temp (the block captures its own name) ---"
    t check: 'recursive fib via a temp-bound block' equals: 55 is: [
        | fib | fib := nil.
        fib := [:n | n < 2 ifTrue: [ n ] ifFalse: [ (fib value: n - 1) + (fib value: n - 2) ] ].
        fib value: 10 ].
    t check: 'recursive factorial via a temp-bound block' equals: 120 is: [
        | fact | fact := nil.
        fact := [:n | n = 0 ifTrue: [ 1 ] ifFalse: [ n * (fact value: n - 1) ] ].
        fact value: 5 ].

    "--- a block is a Block, and it can be sent value repeatedly ---"
    t check: 'block literal is a kind of Block' that: [ | b | b := [ 1 ]. b isKindOf: Block ].
    t check: 'same block returns the same value each call' that: [
        | b | b := [ 6 * 7 ]. (b value = 42) and: [ b value = 42 ] ].

    ^t report
]
