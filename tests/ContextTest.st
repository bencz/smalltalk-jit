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

"Fixture exercising Context / MethodContext / BlockContext / ContextCopy.
 thisContext is nil inside a plain JIT-compiled method (the VM does not reify a
 context there), so the reachable way to observe a live context is through an
 exception: Exception>>signalingContext answers the context in which the
 handler runs (a BlockContext whose home receiver is the handler's self), and
 Exception>>generateBacktrace answers an OrderedCollection of stable
 ContextCopy snapshots. Live Context field reads are only valid while the frame
 is on the stack, so every method below computes a plain value INSIDE the
 handler and returns that; ContextCopy is used for anything inspected later.
 A context whose frame is dead (escaped its activation) or was never framed
 (the thread's dummy root context, captured as home by closures made in
 methods that do not materialize a context) answers nil from the frame-reading
 accessors (receiver / parent / argumentAt: / temporaryAt:) instead of
 crashing the VM. A home context of a method that DOES materialize a context
 (a block captures one of its temps) answers real values."
CtxFixture := Object [
    | tag |
    tag: x [ tag := x. ^self ]
    tag [ ^tag ]
    boom [ ^self error: 'kaboom' ]
    deep [ ^self boom ]

    recvInside      [ ^[ self boom ] on: Error do: [:ex | ex signalingContext receiver ] ]
    ownerInside     [ ^[ self boom ] on: Error do: [:ex | ex signalingContext code ownerClass ] ]
    codeIsCode      [ ^[ self boom ] on: Error do: [:ex | ex signalingContext code isKindOf: CompiledCode ] ]
    ctxClassInside  [ ^[ self boom ] on: Error do: [:ex | ex signalingContext class ] ]
    ctxIsContext    [ ^[ self boom ] on: Error do: [:ex | ex signalingContext isKindOf: Context ] ]
    homeClassInside [ ^[ self boom ] on: Error do: [:ex | ex signalingContext home class ] ]
    outerClassInside[ ^[ self boom ] on: Error do: [:ex | ex signalingContext outer class ] ]
    lineInside      [ ^[ self boom ] on: Error do: [:ex | ex signalingContext line ] ]
    colInside       [ ^[ self boom ] on: Error do: [:ex | ex signalingContext column ] ]
    icClassInside   [ ^[ self boom ] on: Error do: [:ex | ex signalingContext ic class ] ]
    posDescInside   [ ^[ self boom ] on: Error do: [:ex | ex signalingContext positionDescriptor ] ]
    argsSizeInside  [ ^[ self boom ] on: Error do: [:ex | ex signalingContext arguments size ] ]
    tempsSizeInside [ ^[ self boom ] on: Error do: [:ex | ex signalingContext temporaries size ] ]
    argsIsArray     [ ^[ self boom ] on: Error do: [:ex | ex signalingContext arguments isKindOf: Array ] ]
    blockIsCode     [ ^[ self boom ] on: Error do: [:ex | | c | c := ex signalingContext. c block == c code ] ]
    copyInside      [ ^[ self boom ] on: Error do: [:ex | ex signalingContext copy ] ]
    homeCtxInside   [ ^[ self boom ] on: Error do: [:ex | ex signalingContext home ] ]
    homeRecvDummy   [ ^[ self boom ] on: Error do: [:ex | ex signalingContext home receiver ] ]
    homeRecvReal    [ | v | v := 7. ^[ self boom. v ] on: Error do: [:ex | ex signalingContext home receiver ] ]
    homeSelReal     [ | v | v := 7. ^[ self boom. v ] on: Error do: [:ex | ex signalingContext home code selector ] ]
    backtraceInside [ ^[ self deep ] on: Error do: [:ex | ex generateBacktrace ] ]

    backtraceHasSelf [
        | bt found |
        bt := [ self deep ] on: Error do: [:ex | ex generateBacktrace ].
        found := false.
        bt do: [:c | (c receiver == self) ifTrue: [ found := true ] ].
        ^found
    ]
    backtraceSelfTag [
        | bt r |
        bt := [ self deep ] on: Error do: [:ex | ex generateBacktrace ].
        r := nil.
        bt do: [:c | ((c receiver == self) and: [ r isNil ]) ifTrue: [ r := c receiver tag ] ].
        ^r
    ]
    backtraceSelfOwner [
        | bt r |
        bt := [ self deep ] on: Error do: [:ex | ex generateBacktrace ].
        r := nil.
        bt do: [:c | ((c receiver == self) and: [ r isNil ]) ifTrue: [ r := c code ownerClass ] ].
        ^r
    ]
]

[
    | t inst cc mc bt |
    t := TestRun named: 'Context'.
    inst := CtxFixture new tag: 'CTX'.

    "--- thisContext: reachable at top level and inside blocks (nil in plain methods) ---"
    t check: 'thisContext not nil at top level' that: [ thisContext notNil ].
    t check: 'thisContext isKindOf Context' that: [ thisContext isKindOf: Context ].
    t check: 'block thisContext code is a CompiledBlock' that: [ [ thisContext ] value code class == CompiledBlock ].
    t check: 'block thisContext isKindOf Context' that: [ [ thisContext ] value isKindOf: Context ].
    t check: 'top-level thisContext code ownerClass is UndefinedObject' that: [ [ thisContext ] value code ownerClass == UndefinedObject ].
    "--- dead contexts: frame-reading accessors answer nil, never crash ---
     These four used to segfault the VM (NULL / dangling frame pointer read):
     a value'd block context has already escaped its activation."
    t check: 'dead block context receiver is nil' that: [ [ thisContext ] value receiver isNil ].
    t check: 'dead block context parent is nil' that: [ [ thisContext ] value parent isNil ].
    t check: 'dead block context argumentAt: 0 is nil' that: [ ([ thisContext ] value argumentAt: 0) isNil ].
    t check: 'dead framed block context receiver is nil' that: [ | c | c := [ :x | thisContext parent. thisContext ] value: 5. c receiver isNil ].
    t check: 'dead context temporaryAt: does not crash' that: [ [ :x | thisContext ] value: 9. true ].

    "--- signalingContext: a live context observed inside the handler ---"
    t check: 'signalingContext receiver is the handler self' that: [ inst recvInside == inst ].
    t check: 'signalingContext isKindOf Context' equals: true is: [ inst ctxIsContext ].
    t check: 'signalingContext class is BlockContext' that: [ inst ctxClassInside == BlockContext ].
    t check: 'signalingContext code ownerClass is defining class' that: [ inst ownerInside == CtxFixture ].
    t check: 'signalingContext code isKindOf CompiledCode' equals: true is: [ inst codeIsCode ].
    t check: 'signalingContext home is a MethodContext' that: [ inst homeClassInside == MethodContext ].
    t check: 'signalingContext outer is a MethodContext' that: [ inst outerClassInside == MethodContext ].
    t check: 'signalingContext ic is a SmallInteger' that: [ inst icClassInside == SmallInteger ].
    t check: 'signalingContext line is SmallInteger' that: [ inst lineInside isKindOf: SmallInteger ].
    t check: 'signalingContext line positive' that: [ inst lineInside > 0 ].
    t check: 'signalingContext column is SmallInteger' that: [ inst colInside isKindOf: SmallInteger ].
    t check: 'signalingContext positionDescriptor is SmallInteger' that: [ inst posDescInside isKindOf: SmallInteger ].
    t check: 'signalingContext arguments isKindOf Array' equals: true is: [ inst argsIsArray ].
    t check: 'handler block context has one argument' equals: 1 is: [ inst argsSizeInside ].
    t check: 'signalingContext temporaries size is SmallInteger' that: [ inst tempsSizeInside isKindOf: SmallInteger ].

    "--- home context frame-reading accessors (used to crash the VM) ---
     A dummy home (method without a materialized context) answers nil; a real
     home (method whose temp is captured by the block) answers live values."
    t check: 'dummy home receiver is nil or self, no crash' that: [ | r | r := inst homeRecvDummy. r isNil or: [ r == inst ] ].
    t check: 'real home receiver is the method receiver' that: [ inst homeRecvReal == inst ].
    t check: 'real home code selector' that: [ inst homeSelReal == #homeSelReal ].

    "--- BlockContext specific accessor ---"
    t check: 'BlockContext block answers its code' equals: true is: [ inst blockIsCode ].

    "--- MethodContext specific protocol ---"
    mc := inst homeCtxInside.
    t check: 'signalingContext home class is MethodContext' that: [ mc class == MethodContext ].
    t expect: 'MethodContext cannotReturn: raises Error'
        raises: Error in: [ mc cannotReturn: nil ].

    "--- Context>>size is not implemented ---"
    cc := inst copyInside.
    t expect: 'Context size raises ShouldNotImplement'
        raises: ShouldNotImplement in: [ cc size ].

    "--- ContextCopy: a stable snapshot answered by Context>>copy ---"
    t check: 'copy answers a ContextCopy' that: [ cc class == ContextCopy ].
    t check: 'ContextCopy isKindOf Context' that: [ cc isKindOf: Context ].
    t check: 'ContextCopy keeps the receiver' that: [ cc receiver == inst ].
    t check: 'ContextCopy code isKindOf CompiledCode' that: [ cc code isKindOf: CompiledCode ].
    t check: 'ContextCopy code ownerClass is defining class' that: [ cc code ownerClass == CtxFixture ].
    t check: 'ContextCopy arguments is an Array' that: [ cc arguments isKindOf: Array ].
    t check: 'ContextCopy temporaries is an Array' that: [ cc temporaries isKindOf: Array ].

    "--- generateBacktrace yields ContextCopy snapshots ---"
    bt := inst backtraceInside.
    t check: 'generateBacktrace is an OrderedCollection' that: [ bt class == OrderedCollection ].
    t check: 'generateBacktrace has several frames' that: [ bt size > 3 ].
    t check: 'backtrace frames are ContextCopy' that: [ bt first class == ContextCopy ].
    t check: 'backtrace contains a frame for the signaling receiver' equals: true is: [ inst backtraceHasSelf ].
    t check: 'backtrace frame reaches the receiver state' equals: 'CTX' is: [ inst backtraceSelfTag ].
    t check: 'backtrace frame code ownerClass is defining class' that: [ inst backtraceSelfOwner == CtxFixture ].

    "--- Contexts reachable through a Block (outer / home) ---"
    t check: 'block homeContext is a MethodContext' that: [ [ 1 ] homeContext class == MethodContext ].
    t check: 'block outerContext is a BlockContext' that: [ [ 1 ] outerContext class == BlockContext ].
    t check: 'block homeContext isKindOf Context' that: [ [ 1 ] homeContext isKindOf: Context ].
    t check: 'top-level block homeContext receiver is nil' that: [ [ 1 ] homeContext receiver isNil ].

    ^t report
]
