"WARNING: this file deliberately spreads its checks over several top-level
 blocks that share ONE TestRun through the global CFRun. A single driver block
 holding the whole suite aborts the VM at compile time (see the bug note at the
 end of this file), so the suite is partitioned to keep each block's literal
 pool small. There is still exactly one TestRun; the LAST block ends with
 ^t report so its integer count is the process exit code.

 Helper classes.
 RepeatHelper breaks out of an unbounded #repeat with a non-local return.
 UnwindHelper drives ensure:/ifCurtailed: through a non-local return out of the
 enclosing method (the ^ lives in a real method, not a top-level block, because
 a top-level ^ merely ends the eval). Each method logs into the passed
 OrderedCollection so the caller can assert exactly what ran and in what order."

RepeatHelper := Object [
    sumTo: limit [
        | i sum |
        i := 0. sum := 0.
        [ i := i + 1. sum := sum + i. i >= limit ifTrue: [ ^sum ]. true ] repeat.
        ^-1
    ]
]

UnwindHelper := Object [
    normalEnsure: log [
        [ log add: #body ] ensure: [ log add: #cleanup ].
        log add: #after.
        ^#done
    ]
    nlrEnsure: log [
        [ log add: #body. ^#early ] ensure: [ log add: #cleanup ].
        log add: #after.
        ^#normal
    ]
    normalCurtailed: log [
        [ log add: #body ] ifCurtailed: [ log add: #curtailed ].
        log add: #after.
        ^#done
    ]
    nlrCurtailed: log [
        [ log add: #body. ^#early ] ifCurtailed: [ log add: #curtailed ].
        log add: #after.
        ^#normal
    ]
]

"=================== BLOCK 1: looping constructs ==================="
[
    | t |
    t := TestRun named: 'ControlFlow'.
    CFRun := t.

    "--- whileTrue: accumulates while the condition block stays true ---"
    t check: 'whileTrue: accumulates 1..5' equals: 15 is: [
        | i sum | i := 0. sum := 0.
        [ i < 5 ] whileTrue: [ i := i + 1. sum := sum + i ]. sum ].
    t check: 'whileTrue: with false condition runs body zero times' equals: 0 is: [
        | n | n := 0. [ false ] whileTrue: [ n := n + 1 ]. n ].

    "--- whileFalse: accumulates while the condition block stays false ---"
    t check: 'whileFalse: accumulates 1..5' equals: 15 is: [
        | i sum | i := 0. sum := 0.
        [ i >= 5 ] whileFalse: [ i := i + 1. sum := sum + i ]. sum ].
    t check: 'whileFalse: with true condition runs body zero times' equals: 0 is: [
        | n | n := 0. [ true ] whileFalse: [ n := n + 1 ]. n ].

    "--- whileTrue (receiverless): the receiver block yields the condition ---"
    t check: 'whileTrue accumulates 1..5' equals: 15 is: [
        | i sum | i := 0. sum := 0.
        [ i := i + 1. sum := sum + i. i < 5 ] whileTrue. sum ].
    t check: 'whileTrue answers its receiver block' that: [
        | b | b := [ false ]. (b whileTrue) == b ].

    "--- whileFalse (receiverless) ---"
    t check: 'whileFalse accumulates 1..5' equals: 15 is: [
        | i sum | i := 0. sum := 0.
        [ i := i + 1. sum := sum + i. i >= 5 ] whileFalse. sum ].

    "--- repeat: unbounded, exited by a non-local return in a helper method ---"
    t check: 'repeat exits via non-local return' equals: 10 is: [ RepeatHelper new sumTo: 4 ].
    t check: 'repeat with a small bound' equals: 1 is: [ RepeatHelper new sumTo: 1 ].

    "--- timesRepeat: ---"
    t check: 'timesRepeat: runs body N times' equals: 5 is: [
        | n | n := 0. 5 timesRepeat: [ n := n + 1 ]. n ].
    t check: 'timesRepeat: 0 runs body zero times' equals: 0 is: [
        | n | n := 0. 0 timesRepeat: [ n := n + 1 ]. n ].

    "--- to:do: ---"
    t check: 'to:do: sums the inclusive range' equals: 15 is: [
        | sum | sum := 0. 1 to: 5 do: [:i | sum := sum + i]. sum ].
    t check: 'to:do: visits values in order' that: [
        | acc | acc := OrderedCollection new.
        1 to: 4 do: [:i | acc add: i].
        acc asArray = #(1 2 3 4) ].
    t check: 'to:do: with start > stop runs zero times' equals: 0 is: [
        | n | n := 0. 5 to: 1 do: [:i | n := n + 1]. n ].

    "--- to:by:do: with positive and NEGATIVE step ---"
    t check: 'to:by:do: positive step collects odds' that: [
        | acc | acc := OrderedCollection new.
        1 to: 10 by: 2 do: [:i | acc add: i].
        acc asArray = #(1 3 5 7 9) ].
    t check: 'to:by:do: negative step counts down' that: [
        | acc | acc := OrderedCollection new.
        5 to: 1 by: -1 do: [:i | acc add: i].
        acc asArray = #(5 4 3 2 1) ].
    t check: 'to:by:do: negative step by 2' that: [
        | acc | acc := OrderedCollection new.
        10 to: 6 by: -2 do: [:i | acc add: i].
        acc asArray = #(10 8 6) ].
    t check: 'to:by:do: negative step with start < stop runs zero times' equals: 0 is: [
        | n | n := 0. 1 to: 5 by: -1 do: [:i | n := n + 1]. n ].

    "--- detect loops over a collection ---"
    t check: 'detect: finds the first match' equals: 3 is: [
        #(1 2 3 4) detect: [:x | x > 2] ].
    t check: 'detect:ifNone: uses the none block' equals: -1 is: [
        #(1 2 3) detect: [:x | x > 9] ifNone: [ -1 ] ].
    t expect: 'detect: with no match raises NotFoundError' raises: NotFoundError in: [
        | c | c := #(1 2 3). c detect: [:x | x > 9] ].
    t check: 'inject:into: folds with a 2-arg block' equals: 10 is: [
        #(1 2 3 4) inject: 0 into: [:acc :x | acc + x] ].
]

"=================== BLOCK 2: ensure: and ifCurtailed: ==================="
[
    | t |
    t := CFRun.

    "ensure: runs its cleanup in ALL exits (normal, exception unwind, non-local return)."

    "--- (a) normal completion runs the cleanup exactly once ---"
    t check: 'ensure: runs cleanup on normal completion' that: [
        | ran | ran := false.
        [ 1 + 1 ] ensure: [ ran := true ]. ran ].
    t check: 'ensure: answers the protected block value' equals: 42 is: [
        [ 42 ] ensure: [ 99 ] ].
    t check: 'ensure: normal exit order is body then cleanup then after' that: [
        | log | log := OrderedCollection new.
        UnwindHelper new normalEnsure: log.
        log asArray = #(#body #cleanup #after) ].

    "--- (b) an exception unwinding through it, caught OUTSIDE ---"
    t check: 'ensure: cleanup runs while an exception unwinds' that: [
        | log | log := OrderedCollection new.
        [ [ log add: #body. Error signal: 'boom' ] ensure: [ log add: #cleanup ] ]
            on: Error do: [:e | log add: #caught ].
        log includes: #cleanup ].
    t check: 'ensure: exception unwind visits handler then cleanup' that: [
        | log | log := OrderedCollection new.
        [ [ log add: #body. Error signal: 'boom' ] ensure: [ log add: #cleanup ] ]
            on: Error do: [:e | log add: #caught ].
        log asArray = #(#body #caught #cleanup) ].

    "--- (c) a non-local return out of the enclosing method ---"
    t check: 'ensure: cleanup runs on non-local return' that: [
        | log res | log := OrderedCollection new.
        res := UnwindHelper new nlrEnsure: log.
        (res = #early) and: [ log asArray = #(#body #cleanup) ] ].

    "ifCurtailed: runs its block ONLY on an abnormal exit, never on normal completion."

    "--- normal completion does NOT run the curtailed block ---"
    t check: 'ifCurtailed: does not run on normal completion' that: [
        | ran | ran := false.
        [ 1 + 1 ] ifCurtailed: [ ran := true ]. ran not ].
    t check: 'ifCurtailed: answers the protected block value' equals: 5 is: [
        [ 5 ] ifCurtailed: [ 99 ] ].
    t check: 'ifCurtailed: normal exit does not log curtailed' that: [
        | log | log := OrderedCollection new.
        UnwindHelper new normalCurtailed: log.
        log asArray = #(#body #after) ].

    "--- an exception unwinding through it DOES run the curtailed block ---"
    t check: 'ifCurtailed: runs while an exception unwinds' that: [
        | log | log := OrderedCollection new.
        [ [ log add: #body. Error signal: 'boom' ] ifCurtailed: [ log add: #curtailed ] ]
            on: Error do: [:e | log add: #caught ].
        log includes: #curtailed ].

    "--- a non-local return out of the enclosing method runs the curtailed block ---"
    t check: 'ifCurtailed: runs on non-local return' that: [
        | log res | log := OrderedCollection new.
        res := UnwindHelper new nlrCurtailed: log.
        (res = #early) and: [ log asArray = #(#body #curtailed) ] ].
]

"=========== BLOCK 3: boolean algebra, conditionals, nil handling ==========="
[
    | t |
    t := CFRun.

    "--- not ---"
    t check: 'true not' equals: false is: [ true not ].
    t check: 'false not' equals: true is: [ false not ].

    "--- & (non short-circuit and) full truth table ---"
    t check: 'true & true' that: [ true & true ].
    t check: 'true & false is false' that: [ (true & false) not ].
    t check: 'false & true is false' that: [ (false & true) not ].
    t check: 'false & false is false' that: [ (false & false) not ].

    "--- | (non short-circuit or) full truth table ---"
    t check: 'true | false' that: [ true | false ].
    t check: 'false | true' that: [ false | true ].
    t check: 'false | false is false' that: [ (false | false) not ].
    t check: 'true | true' that: [ true | true ].

    "--- and: / or: truth tables ---"
    t check: 'true and: true' that: [ true and: [ true ] ].
    t check: 'true and: false is false' that: [ (true and: [ false ]) not ].
    t check: 'false and: true is false' that: [ (false and: [ true ]) not ].
    t check: 'true or: false' that: [ true or: [ false ] ].
    t check: 'false or: true' that: [ false or: [ true ] ].
    t check: 'false or: false is false' that: [ (false or: [ false ]) not ].

    "--- short-circuit: the argument block must NOT be evaluated ---"
    t check: 'and: skips its arg when receiver is false' that: [
        | evaluated r | evaluated := false.
        r := false and: [ evaluated := true. true ].
        (r = false) and: [ evaluated = false ] ].
    t check: 'or: skips its arg when receiver is true' that: [
        | evaluated r | evaluated := false.
        r := true or: [ evaluated := true. false ].
        (r = true) and: [ evaluated = false ] ].
    t check: 'and: DOES evaluate its arg when receiver is true' that: [
        | evaluated | evaluated := false.
        true and: [ evaluated := true. true ].
        evaluated ].
    t check: 'or: DOES evaluate its arg when receiver is false' that: [
        | evaluated | evaluated := false.
        false or: [ evaluated := true. false ].
        evaluated ].

    "--- xor: (eqv: is not implemented in this kernel, so it is not tested) ---"
    t check: 'true xor: false' that: [ true xor: false ].
    t check: 'false xor: true' that: [ false xor: true ].
    t check: 'true xor: true is false' that: [ (true xor: true) not ].
    t check: 'false xor: false is false' that: [ (false xor: false) not ].

    "--- ifTrue: / ifFalse: single-armed ---"
    t check: 'ifTrue: fires on true' equals: #yes is: [ true ifTrue: [ #yes ] ].
    t check: 'ifFalse: fires on false' equals: #yes is: [ false ifFalse: [ #yes ] ].

    "--- a one-armed conditional that does not fire answers nil ---"
    t check: 'false ifTrue: answers nil' that: [ (false ifTrue: [ #x ]) isNil ].
    t check: 'true ifFalse: answers nil' that: [ (true ifFalse: [ #x ]) isNil ].

    "--- two-armed ifTrue:ifFalse: and ifFalse:ifTrue: ---"
    t check: 'ifTrue:ifFalse: true arm' equals: #t is: [ true ifTrue: [ #t ] ifFalse: [ #f ] ].
    t check: 'ifTrue:ifFalse: false arm' equals: #f is: [ false ifTrue: [ #t ] ifFalse: [ #f ] ].
    t check: 'ifFalse:ifTrue: true arm' equals: #t is: [ true ifFalse: [ #f ] ifTrue: [ #t ] ].
    t check: 'ifFalse:ifTrue: false arm' equals: #f is: [ false ifFalse: [ #f ] ifTrue: [ #t ] ].

    "--- ifNil: / ifNotNil: single-armed ---"
    t check: 'nil ifNil: fires' equals: #wasNil is: [ nil ifNil: [ #wasNil ] ].
    t check: '5 ifNil: does not fire, answers receiver' equals: 5 is: [ 5 ifNil: [ #wasNil ] ].
    t check: '42 ifNotNil: fires with the receiver bound' equals: 43 is: [ 42 ifNotNil: [:x | x + 1] ].
    t check: 'nil ifNotNil: does not fire, answers nil' that: [ (nil ifNotNil: [:x | x]) isNil ].

    "--- two-armed ifNil:ifNotNil: and ifNotNil:ifNil: ---"
    t check: 'ifNil:ifNotNil: nil branch' equals: #n is: [ nil ifNil: [ #n ] ifNotNil: [:x | x] ].
    t check: 'ifNil:ifNotNil: notNil branch binds receiver' equals: 5 is: [ 5 ifNil: [ #n ] ifNotNil: [:x | x] ].
    t check: 'ifNotNil:ifNil: notNil branch binds receiver' equals: 5 is: [ 5 ifNotNil: [:x | x] ifNil: [ #n ] ].
    t check: 'ifNotNil:ifNil: nil branch' equals: #n is: [ nil ifNotNil: [:x | x] ifNil: [ #n ] ].

    "--- isNil / notNil ---"
    t check: 'nil isNil' that: [ nil isNil ].
    t check: 'nil notNil is false' that: [ nil notNil not ].
    t check: 'object isNil is false' that: [ 5 isNil not ].
    t check: 'object notNil' that: [ 5 notNil ].

    ^t report
]

"BUG NOTE (VM compilation ceiling, why this file is split into 3 blocks).
 Putting the entire suite in ONE top-level driver block aborts the VM at
 compile/JIT time, before any check runs:

   Assertion '(value & 3) == VALUE_POINTER' failed in vm/core/Object.h:287
   (SIGABRT, process exit 134)

 It is driven by the number of DISTINCT LITERALS in a single method/block, not
 by the check count. A block of ~70 checks that all reuse the same literals
 compiles fine; a block of ~60 checks each introducing fresh literal symbols and
 literal arrays crosses the ceiling (a block of ~40 such checks is still fine).
 The threshold sits near 256 distinct literals, which looks like an unchecked
 literal-index limit: once it overflows, the compiler emits a corrupt literal
 reference that the interpreter/GC later reads as a non-pointer, tripping the
 VALUE_POINTER assert. Minimal repro: a single block with 60 checks of the shape
   t check: 'cK' that: [ | log | log := OrderedCollection new. log add: #symK.
       (log asArray = #(#symK)) and: [ #(#aK #bK) includes: #aK ] ].
 aborts; the same 60 checks split across 3 blocks pass. Splitting the driver
 keeps every check green while staying under the per-method ceiling."
