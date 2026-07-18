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

"ErrorHierarchyTest: the exception CLASS graph and handler SELECTION, which the
 existing exception tests do not cover directly. Two facts drive most of it:
   * Error is resumable-false and is the parent of MessageNotUnderstood,
     OutOfRangeError, IoError, SubClassResponsibility and ShouldNotImplement.
   * Warning and NotFoundError are direct Exception subclasses, NOT Errors, so
     an `on: Error do:` handler must NOT catch them while `on: Exception do:`
     must. Handler selection is verified by letting a non-matching inner handler
     fall through to an outer `on: Exception` sentinel.

 A custom class tree (base -> mid -> leaf, plus a sibling and a direct Exception
 subclass) probes user-defined selection. Globals live in this test process
 only."
HErrBase := Error [
]
HErrMid := HErrBase [
]
HErrLeaf := HErrMid [
]
HErrSibling := Error [
]
HExcCustom := Exception [
]

ErrHierProbe := Object [

    "Built-in class relationships via inheritsFrom:."
    class fillClassHierarchy: t [
        t check: 'Error inheritsFrom Exception'
            that: [ Error inheritsFrom: Exception ].
        t check: 'Warning inheritsFrom Exception'
            that: [ Warning inheritsFrom: Exception ].
        t check: 'Warning does NOT inheritFrom Error'
            that: [ (Warning inheritsFrom: Error) not ].
        t check: 'MessageNotUnderstood inheritsFrom Error'
            that: [ MessageNotUnderstood inheritsFrom: Error ].
        t check: 'NotFoundError inheritsFrom Exception'
            that: [ NotFoundError inheritsFrom: Exception ].
        t check: 'NotFoundError does NOT inheritFrom Error'
            that: [ (NotFoundError inheritsFrom: Error) not ].
        t check: 'OutOfRangeError inheritsFrom Error'
            that: [ OutOfRangeError inheritsFrom: Error ].
        t check: 'IoError inheritsFrom Error'
            that: [ IoError inheritsFrom: Error ].
        t check: 'SubClassResponsibility inheritsFrom Error'
            that: [ SubClassResponsibility inheritsFrom: Error ].
        t check: 'ShouldNotImplement inheritsFrom Error'
            that: [ ShouldNotImplement inheritsFrom: Error ].
    ]

    "Instance-level isKindOf: relationships."
    class fillIsKindOf: t [
        t check: 'NotFoundError instance isKindOf Exception'
            that: [ NotFoundError new isKindOf: Exception ].
        t check: 'NotFoundError instance is NOT a kind of Error'
            that: [ (NotFoundError new isKindOf: Error) not ].
        t check: 'OutOfRangeError instance isKindOf Error'
            that: [ (OutOfRangeError value: 1 between: 2 and: 3) isKindOf: Error ].
        t check: 'MNU instance isKindOf Error and Exception'
            that: [ (MessageNotUnderstood new isKindOf: Error)
                    and: [MessageNotUnderstood new isKindOf: Exception] ].
        t check: 'Error instance isKindOf Exception'
            that: [ Error new isKindOf: Exception ].
    ]

    "Handler class SELECTION. A non-matching inner handler must fall through to
     the outer on: Exception sentinel."
    class fillHandlerSelection: t [
        t check: 'on: Error catches an Error'
            equals: #caught
            is: [ [Error signal] on: Error do: [:e | #caught] ].
        t check: 'on: Error does NOT catch a Warning'
            equals: #byException
            is: [ [[Warning signal] on: Error do: [:e | #byError]]
                    on: Exception do: [:e | #byException] ].
        t check: 'on: Error does NOT catch a NotFoundError'
            equals: #byException
            is: [ [[NotFoundError signal] on: Error do: [:e | #byError]]
                    on: Exception do: [:e | #byException] ].
        t check: 'on: Exception catches an Error'
            equals: #ok
            is: [ [Error signal] on: Exception do: [:e | #ok] ].
        t check: 'on: Exception catches a Warning'
            that: [ [Warning signal] on: Exception do: [:e | e class == Warning] ].
        t check: 'on: Exception catches a NotFoundError'
            that: [ [NotFoundError signal] on: Exception do: [:e | e class == NotFoundError] ].
        t check: 'on: Exception catches a MessageNotUnderstood'
            that: [ | r | r := Object new.
                    [r missingSelectorHere. false] on: Exception do: [:e | e isKindOf: MessageNotUnderstood] ].
    ]

    "Exception class>>handles: mirrors isKindOf:."
    class fillHandles: t [
        t check: 'Error handles: an Error instance'
            that: [ Error handles: Error new ].
        t check: 'Error does NOT handle a Warning instance'
            that: [ (Error handles: Warning new) not ].
        t check: 'Exception handles: a Warning instance'
            that: [ Exception handles: Warning new ].
        t check: 'Error handles: a MessageNotUnderstood instance'
            that: [ Error handles: MessageNotUnderstood new ].
        t check: 'Error does NOT handle a NotFoundError instance'
            that: [ (Error handles: NotFoundError new) not ].
        t check: 'NotFoundError handles: a NotFoundError instance'
            that: [ NotFoundError handles: NotFoundError new ].
    ]

    "User-defined subclass tree: base -> mid -> leaf, a sibling, and a direct
     Exception subclass."
    class fillCustomTree: t [
        t check: 'custom leaf caught by a handler on itself'
            equals: #self
            is: [ [HErrLeaf signal] on: HErrLeaf do: [:e | #self] ].
        t check: 'custom leaf caught by a handler on its mid ancestor'
            equals: #mid
            is: [ [HErrLeaf signal] on: HErrMid do: [:e | #mid] ].
        t check: 'custom leaf caught by a handler on its base ancestor'
            equals: #base
            is: [ [HErrLeaf signal] on: HErrBase do: [:e | #base] ].
        t check: 'custom leaf caught by on: Error'
            equals: #err
            is: [ [HErrLeaf signal] on: Error do: [:e | #err] ].
        t check: 'custom leaf caught by on: Exception'
            equals: #exc
            is: [ [HErrLeaf signal] on: Exception do: [:e | #exc] ].
        t check: 'a sibling class does NOT catch the base exception'
            equals: #outer
            is: [ [[HErrBase signal] on: HErrSibling do: [:e | #wrong]]
                    on: Exception do: [:e | #outer] ].
        t check: 'a descendant handler does NOT catch an ancestor signal'
            equals: #outer
            is: [ [[HErrBase signal] on: HErrLeaf do: [:e | #wrong]]
                    on: Exception do: [:e | #outer] ].
        t check: 'custom leaf instance isKindOf its base and Error'
            that: [ (HErrLeaf new isKindOf: HErrBase) and: [HErrLeaf new isKindOf: Error] ].
        t check: 'custom direct Exception subclass is NOT caught by on: Error'
            equals: #outer
            is: [ [[HExcCustom signal] on: Error do: [:e | #wrong]]
                    on: Exception do: [:e | #outer] ].
        t check: 'custom direct Exception subclass inherits resumable-true'
            that: [ HExcCustom new isResumable ].
        t check: 'custom Error subclass inherits resumable-false'
            that: [ HErrBase new isResumable not ].
    ]
]

[
    | t |
    t := TestRun named: 'ErrorHierarchy'.
    ErrHierProbe fillClassHierarchy: t.
    ErrHierProbe fillIsKindOf: t.
    ErrHierProbe fillHandlerSelection: t.
    ErrHierProbe fillHandles: t.
    ErrHierProbe fillCustomTree: t.
    ^t report
]
