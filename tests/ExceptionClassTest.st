"ExceptionClassTest: signaling, message text, the Object error protocol
 (error:/doesNotUnderstand:/subclassResponsibility/shouldNotImplement), the
 resumable Warning default, and the instance-level handler protocol
 (return:/return/retry/retryUsing:/pass/outer/resignalAs:/resume:/ensure:/
 ifCurtailed:). Complements the existing Assert-based ExceptionProtocolTest and
 ResumableTest by exercising the SAME protocol through the fail-loud harness and
 by adding the class-level error entry points that those files do not touch.

 Custom exception classes for the resignalAs: and default-text probes. Defined
 at file scope (they become globals in this test process only)."
ExcCustomErr := Error [
]
ExcSignalA := Error [
]
ExcSignalB := Error [
]

ExcClassProbe := Object [

    "Signaling and message text."
    class fillMessages: t [
        t check: 'signal: sets messageText'
            equals: 'boom'
            is: [ [Error signal: 'boom'] on: Error do: [:e | e messageText] ].
        t check: 'new signal: sets messageText'
            equals: 'later-x'
            is: [ [Error new signal: 'later-x'] on: Error do: [:e | e messageText] ].
        t check: 'description equals messageText'
            that: [ [Error signal: 'desc'] on: Error do: [:e | e description = 'desc'] ].
        t check: 'bare signal messageText falls back to class name'
            that: [ [Error signal] on: Error do: [:e | e messageText = e class name] ].
        t check: 'defaultMessageText of custom class is its class name'
            that: [ [ExcCustomErr signal] on: Error do: [:e | e messageText = ExcCustomErr name] ].
        t check: 'messageText: setter is honoured at signal'
            equals: 'set-before-signal'
            is: [ | ex | ex := Error new. ex messageText: 'set-before-signal'.
                  [ex signal] on: Error do: [:e | e messageText] ].
        t check: 'handler receives the very instance that was signaled'
            that: [ | inst | inst := Error new. [inst signal] on: Error do: [:e | e == inst] ].
        t check: 'caught exception class is Error'
            that: [ [Error signal] on: Error do: [:e | e class == Error] ].
        t check: 'signalingContext is available inside handler'
            that: [ [Error signal] on: Error do: [:e | e signalingContext notNil] ].
        t check: 'generateBacktrace yields a non-empty collection'
            that: [ [Error signal] on: Error do: [:e | e generateBacktrace isEmpty not] ].
    ]

    "Object>>error:/subclassResponsibility/shouldNotImplement."
    class fillObjectProtocol: t [
        t expect: 'error: raises an Error'
            raises: Error
            in: [ Object new error: 'anything' ].
        t check: 'error: messageText is the passed string'
            equals: 'explicit-message'
            is: [ [Object new error: 'explicit-message'] on: Error do: [:e | e messageText] ].
        t expect: 'subclassResponsibility raises SubClassResponsibility'
            raises: SubClassResponsibility
            in: [ Object new subclassResponsibility ].
        t check: 'subclassResponsibility is an Error'
            that: [ [Object new subclassResponsibility] on: Error do: [:e | e isKindOf: Error] ].
        t check: 'subclassResponsibility default text'
            equals: 'called method is responsibility of sub class'
            is: [ [Object new subclassResponsibility] on: Error do: [:e | e messageText] ].
        t expect: 'shouldNotImplement raises ShouldNotImplement'
            raises: ShouldNotImplement
            in: [ Object new shouldNotImplement ].
        t check: 'shouldNotImplement default text'
            equals: 'method should not be implemented'
            is: [ [Object new shouldNotImplement] on: Error do: [:e | e messageText] ].
    ]

    "doesNotUnderstand: -> MessageNotUnderstood. Receiver is bound to a temp
     first (never a literal): a DNU sent straight to a literal receiver and then
     resumed past crashes the VM (documented), so this stays on temps."
    class fillDnu: t [
        t expect: 'DNU raises MessageNotUnderstood'
            raises: MessageNotUnderstood
            in: [ | r | r := Object new. r noSuchSelectorHere ].
        t check: 'MNU captures the unary selector'
            that: [ | r | r := Object new.
                    [r missingUnaryProbe. false] on: MessageNotUnderstood do: [:e | e message selector == #missingUnaryProbe] ].
        t check: 'MNU captures the keyword selector'
            that: [ | r | r := Object new.
                    [r missingA: 1 b: 2. false] on: MessageNotUnderstood do: [:e | e message selector == #missingA:b:] ].
        t check: 'MNU captures the receiver'
            that: [ | r | r := Object new.
                    [r missingUnaryProbe. false] on: MessageNotUnderstood do: [:e | e receiver == r] ].
        t check: 'MNU message arguments arity matches the selector'
            equals: 2
            is: [ | r | r := Object new.
                  [r missingA: 1 b: 2. -1] on: MessageNotUnderstood do: [:e | e message arguments size] ].
        t check: 'MNU is a kind of Error'
            that: [ | r | r := Object new.
                    [r missingUnaryProbe. false] on: MessageNotUnderstood do: [:e | e isKindOf: Error] ].
        t check: 'a MNU is caught by a plain on: Error handler'
            that: [ | r | r := Object new.
                    [r missingUnaryProbe. false] on: Error do: [:e | e isKindOf: MessageNotUnderstood] ].
    ]

    "Warning: the resumable notification. Exercised through explicit handlers so
     the harness on: Exception wrapper never swallows an unhandled Warning; the
     resume-nil default is probed directly via defaultAction."
    class fillWarning: t [
        t check: 'Warning instances are resumable'
            that: [ Warning new isResumable ].
        t check: 'Error instances are not resumable'
            that: [ Error new isResumable not ].
        t check: 'a handled Warning can be resumed with a value'
            equals: 5
            is: [ [Warning signal: 'w'] on: Warning do: [:e | e resume: 5] ].
        t check: 'Warning defaultAction answers nil (resume nil semantics)'
            that: [ (Warning new messageText: 'intentional warning probe (ExceptionClassTest)') defaultAction isNil ].
        t expect: 'resume: on a non-resumable Error raises an Error'
            raises: Error
            in: [ [Error signal] on: Error do: [:e | e resume: 1] ].
    ]

    "Instance handler protocol: return:/return/retry/retryUsing:/pass/outer/
     resignalAs:/ensure:/ifCurtailed:. Every retry is bounded by a counter."
    class fillHandlerProtocol: t [
        t check: 'return: makes on:do: answer the value'
            equals: 77
            is: [ [Error signal] on: Error do: [:e | e return: 77] ].
        t check: 'return answers nil'
            that: [ ([Error signal] on: Error do: [:e | e return]) isNil ].
        t check: 'retry re-runs the protected block until it stops signaling'
            equals: 3
            is: [ | c | c := 0.
                  [c := c + 1. c < 3 ifTrue: [Error signal]. c] on: Error do: [:e | e retry] ].
        t check: 'retryUsing: evaluates the replacement block'
            equals: #replaced
            is: [ [Error signal] on: Error do: [:e | e retryUsing: [#replaced]] ].
        t check: 'pass re-raises to the enclosing handler'
            equals: #outer
            is: [ [[Error signal] on: Error do: [:e | e pass]] on: Error do: [:e | #outer] ].
        t check: 'outer answers the enclosing handler resumption inside the inner handler'
            equals: 41
            is: [ [[Warning signal] on: Warning do: [:e | e outer + 1]]
                    on: Warning do: [:e2 | e2 resume: 40] ].
        t check: 'resignalAs: hands off to a handler for the replacement class'
            equals: #resignaled
            is: [ [[ExcSignalA signal] on: ExcSignalA do: [:e | e resignalAs: ExcSignalB new]]
                    on: ExcSignalB do: [:e2 | #resignaled] ].
        t check: 'ensure: cleanup runs while unwinding through an exception'
            that: [ | log | log := OrderedCollection new.
                    [[Error signal] ensure: [log add: #cleanup]] on: Error do: [:e | nil].
                    log includes: #cleanup ].
        t check: 'ifCurtailed: does not fire on normal completion'
            that: [ | log | log := OrderedCollection new.
                    [42] ifCurtailed: [log add: #ran].
                    log isEmpty ].
        t check: 'ifCurtailed: fires when the block is curtailed by an exception'
            that: [ | log | log := OrderedCollection new.
                    [[Error signal] ifCurtailed: [log add: #ran]] on: Error do: [:e | nil].
                    log includes: #ran ].
    ]
]

[
    | t |
    t := TestRun named: 'ExceptionClass'.
    ExcClassProbe fillMessages: t.
    ExcClassProbe fillObjectProtocol: t.
    ExcClassProbe fillDnu: t.
    ExcClassProbe fillWarning: t.
    ExcClassProbe fillHandlerProtocol: t.
    ^t report
]
