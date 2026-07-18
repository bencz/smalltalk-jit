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

"Fixture exercising CompiledMethod / CompiledBlock / CompiledCode reflectively.
 Methods are obtained through Behavior>>compiledMethodAt: and
 Behavior>>lookupSelector: (there is no >> operator in this image). Compiled
 blocks are reached through Block>>compiledBlock."
CMTFixture := Object [
    | iv1 iv2 |
    noArg [ ^42 ]
    oneArg: x [ ^x ]
    twoArgs: x and: y [ | t | t := x + y. ^t ]
    withTemps [ | p q r | p := 1. q := 2. r := 3. ^p + q + r ]
    primNew [ <primitive: BehaviorNewPrimitive> ^nil ]
    nlr [ #(1 2 3) do: [:x | x = 2 ifTrue: [ ^x ] ]. ^0 ]
    add: a to: b [ ^a + b ]
    blockCode [ ^[ :n | n * 2 ] compiledBlock ]
    class classSide [ ^7 ]
]

[
    | t inst mNo mOne mTwo mTemps mPrim mNlr mAdd mCls cbTop cb0 cb1 cb3 cbMethod |
    t := TestRun named: 'CompiledMethod'.
    inst := CMTFixture new.

    mNo    := CMTFixture compiledMethodAt: #noArg.
    mOne   := CMTFixture compiledMethodAt: #oneArg:.
    mTwo   := CMTFixture compiledMethodAt: #twoArgs:and:.
    mTemps := CMTFixture compiledMethodAt: #withTemps.
    mPrim  := CMTFixture compiledMethodAt: #primNew.
    mNlr   := CMTFixture compiledMethodAt: #nlr.
    mAdd   := CMTFixture compiledMethodAt: #add:to:.
    mCls   := CMTFixture class compiledMethodAt: #classSide.

    "--- obtaining a method / class identity ---"
    t check: 'compiledMethodAt returns CompiledMethod' that: [ mNo class == CompiledMethod ].
    t check: 'method isKindOf CompiledCode' that: [ mNo isKindOf: CompiledCode ].

    "--- selector ---"
    t check: 'selector unary' equals: #noArg is: [ mNo selector ].
    t check: 'selector keyword' equals: #twoArgs:and: is: [ mTwo selector ].
    t check: 'selector class-side' equals: #classSide is: [ mCls selector ].

    "--- ownerClass / methodClass ---"
    t check: 'ownerClass is defining class' that: [ mNo ownerClass == CMTFixture ].
    t check: 'ownerClass keyword method' that: [ mTwo ownerClass == CMTFixture ].
    t check: 'class-side ownerClass is metaclass' that: [ mCls ownerClass == CMTFixture class ].

    "--- argumentsSize across arities ---"
    t check: 'argumentsSize 0' equals: 0 is: [ mNo argumentsSize ].
    t check: 'argumentsSize 1' equals: 1 is: [ mOne argumentsSize ].
    t check: 'argumentsSize 2' equals: 2 is: [ mTwo argumentsSize ].
    t check: 'argumentsSize add:to: is 2' equals: 2 is: [ mAdd argumentsSize ].
    t check: 'more args means larger argumentsSize' that: [ mTwo argumentsSize > mNo argumentsSize ].

    "--- temporariesSize ---"
    t check: 'temporariesSize 0 for no temps' equals: 0 is: [ mNo temporariesSize ].
    t check: 'temporariesSize 1 for one temp' equals: 1 is: [ mTwo temporariesSize ].
    t check: 'temporariesSize positive with temps' that: [ mTemps temporariesSize > 0 ].
    t check: 'temporariesSize isSmallInteger' that: [ mTemps temporariesSize isKindOf: SmallInteger ].

    "--- header ---"
    t check: 'header is SmallInteger' that: [ mNo header isKindOf: SmallInteger ].
    t check: 'header zero for trivial method' equals: 0 is: [ mNo header ].
    t check: 'header nonzero when it carries args' that: [ mTwo header > 0 ].

    "--- hasContext / outerReturns are Booleans, differential ---"
    t check: 'hasContext is Boolean' that: [ mNo hasContext isKindOf: Boolean ].
    t check: 'plain method hasContext false' equals: false is: [ mNo hasContext ].
    t check: 'non-local-return method hasContext true' equals: true is: [ mNlr hasContext ].
    t check: 'outerReturns is Boolean' that: [ mNo outerReturns isKindOf: Boolean ].
    t check: 'plain method outerReturns false' equals: false is: [ mNo outerReturns ].

    "--- primitive ---"
    t check: 'primitive nil for a plain method' that: [ mNo primitive isNil ].
    t check: 'primitive non-nil for a primitive method' that: [ mPrim primitive notNil ].
    t check: 'primitive number is SmallInteger' that: [ mPrim primitive isKindOf: SmallInteger ].

    "--- literals / sourceCode / descriptors ---"
    t check: 'literals is an Array' that: [ mNo literals isKindOf: Array ].
    t check: 'sourceCode not nil' that: [ mNo sourceCode notNil ].
    t check: 'descriptors is an Array' that: [ mNo descriptors isKindOf: Array ].

    "--- printString ---"
    t check: 'printString shows class#selector' equals: 'a CMTFixture#noArg' is: [ mNo printString ].
    t check: 'printString of keyword method' equals: 'a CMTFixture#twoArgs:and:' is: [ mTwo printString ].

    "--- equality / identity ---"
    t check: 'method == itself' that: [ mNo == mNo ].
    t check: 'method = itself' that: [ mNo = mNo ].
    t check: 'compiledMethodAt returns same object' that: [ (CMTFixture compiledMethodAt: #noArg) == mNo ].
    t check: 'different methods not equal' that: [ (mNo = mAdd) not ].

    "--- reflective invocation via sendTo: ---"
    t check: 'sendTo: runs the method' equals: 42 is: [ mNo sendTo: inst ].
    t check: 'sendTo:withArguments: passes args' equals: 7 is: [ mAdd sendTo: inst withArguments: #(3 4) ].

    "--- source-derived argument / temporary names ---"
    t check: 'arguments names count matches argumentsSize' that: [ mTwo arguments size = mTwo argumentsSize ].
    t check: 'arguments names empty for no-arg method' equals: 0 is: [ mNo arguments size ].
    t check: 'first argument name' equals: 'x' is: [ mTwo arguments first ].
    t check: 'temporary name present' equals: 't' is: [ mTwo temporaries first ].

    "--- lookupSelector / missing selector ---"
    t check: 'lookupSelector present returns CompiledMethod' that: [ (CMTFixture lookupSelector: #noArg) class == CompiledMethod ].
    t check: 'lookupSelector missing returns nil' that: [ (CMTFixture lookupSelector: #totallyBogusXYZ) isNil ].
    t expect: 'compiledMethodAt missing raises NotFoundError'
        raises: NotFoundError in: [ CMTFixture compiledMethodAt: #totallyBogusXYZ ].

    "--- CompiledBlock from block literals of several arities ---"
    cbTop := [:a :b | a + b] compiledBlock.
    cb0   := [ 1 ] compiledBlock.
    cb1   := [:z | z ] compiledBlock.
    cb3   := [:a :b :c | a ] compiledBlock.
    t check: 'compiledBlock class is CompiledBlock' that: [ cbTop class == CompiledBlock ].
    t check: 'CompiledBlock isKindOf CompiledCode' that: [ cbTop isKindOf: CompiledCode ].
    t check: 'block argumentsSize 0' equals: 0 is: [ cb0 argumentsSize ].
    t check: 'block argumentsSize 1' equals: 1 is: [ cb1 argumentsSize ].
    t check: 'block argumentsSize 2' equals: 2 is: [ cbTop argumentsSize ].
    t check: 'block argumentsSize 3' equals: 3 is: [ cb3 argumentsSize ].
    t check: 'block temporariesSize is SmallInteger' that: [ cbTop temporariesSize isKindOf: SmallInteger ].
    t check: 'block header is SmallInteger' that: [ cbTop header isKindOf: SmallInteger ].
    t check: 'block hasContext is Boolean' that: [ cbTop hasContext isKindOf: Boolean ].
    t check: 'block primitive nil' that: [ cbTop primitive isNil ].
    t check: 'block literals is an Array' that: [ cbTop literals isKindOf: Array ].
    t check: 'top-level block ownerClass is UndefinedObject' that: [ cbTop ownerClass == UndefinedObject ].

    "--- CompiledBlock defined inside a method knows its class ---"
    cbMethod := inst blockCode.
    t check: 'method-block compiledBlock class' that: [ cbMethod class == CompiledBlock ].
    t check: 'method-block ownerClass is defining class' that: [ cbMethod ownerClass == CMTFixture ].
    t check: 'method-block argumentsSize' equals: 1 is: [ cbMethod argumentsSize ].
    t check: 'method-block method is a CompiledMethod' that: [ cbMethod method class == CompiledMethod ].
    t check: 'method-block method ownerClass' that: [ cbMethod method ownerClass == CMTFixture ].
    t check: 'method-block literals delegate to method' that: [ cbMethod literals == cbMethod method literals ].
    t check: 'method-block printString ends with []' equals: 'a CMTFixture#blockCode[]' is: [ cbMethod printString ].

    ^t report
]
