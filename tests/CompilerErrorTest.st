"=== CompilerErrorTest =========================================================
 Exercises the Compiler happy path and its error paths, driving the compiler
 PROGRAMMATICALLY so every malformed input is catchable inside a check block.

 Entry points used (same family CompilerTest.st uses):
   Compiler new evaluate: aSourceString
   Compiler new evaluate: aSourceString withArguments: aDictionary
   Compiler new buildClass: (Parser parseString: aSourceString) parseClass

 Error classes observed by probing (each asserted below):
   ParseError               syntax errors
   UndefinedVariableError   lowercase undefined name, or undefined superclass
   ReadonlyVariableError    assignment to self/true/nil/super or to a formal arg
   RedefinitionError        duplicate temp/arg, or duplicate method selector
   InvalidPragmaError       class <shape:> pragma with a non var/integer arg
   MessageNotUnderstood     RUNTIME error (an uppercase undefined global is nil,
                            so the message goes to nil), NOT a compile error
 =============================================================================="
[
    | t c argsDict |
    t := TestRun named: 'CompilerError'.
    c := Compiler new.

    "----- happy path: evaluate returns the last expression's value -----"
    t check: 'evaluate 3 + 4' equals: 7 is: [ c evaluate: '3 + 4' ].
    t check: 'evaluate explicit return' equals: 20 is: [ c evaluate: '^ 10 * 2' ].
    t check: 'evaluate with temps' equals: 15 is: [ c evaluate: '| a | a := 5. a * 3' ].
    t check: 'evaluate two temps' equals: 12 is: [ c evaluate: '| a b | a := 3. b := 4. a * b' ].
    t check: 'evaluate string concat' equals: 'abcd' is: [ c evaluate: '''ab'', ''cd''' ].
    t check: 'evaluate block value' equals: 36 is: [ c evaluate: '[:x | x * x] value: 6' ].
    t check: 'evaluate array at' equals: 20 is: [ c evaluate: '#(10 20 30) at: 2' ].
    t check: 'evaluate boolean' equals: true is: [ c evaluate: '3 > 2' ].

    "----- happy path: evaluate:withArguments: -----"
    argsDict := Dictionary new.
    argsDict at: 'x' put: 10.
    t check: 'evaluate withArguments' equals: 15 is: [ c evaluate: 'x + 5' withArguments: argsDict ].

    "----- happy path: build a class from a string, then send to it -----"
    t check: 'buildClass returns class' that: [
        | cls |
        cls := c buildClass: (Parser parseString:
            'CmpErrGoodCls := Object [ | n | setN: v [ n := v ] doubleN [ ^n * 2 ] ]') parseClass.
        cls == CmpErrGoodCls ].
    t check: 'built class instance responds' equals: 42 is: [
        | o | o := CmpErrGoodCls new. o setN: 21. o doubleN ].
    t check: 'built class class-variable' equals: 99 is: [
        | o |
        c buildClass: (Parser parseString:
            'CmpErrCVarCls := Object [ | ClassV | set [ ClassV := 99 ] get [ ^ClassV ] ]') parseClass.
        o := CmpErrCVarCls new. o set. o get ].
    t check: 'unrecognized class pragma is ignored' equals: 7 is: [
        c buildClass: (Parser parseString:
            'CmpErrPragCls := Object [ <category: ''demo''> foo [ ^7 ] ]') parseClass.
        CmpErrPragCls new foo ].

    "----- error path: ParseError from malformed syntax -----"
    t expect: 'trailing binary op' raises: ParseError in: [ c evaluate: '3 + ' ].
    t expect: 'unbalanced paren' raises: ParseError in: [ c evaluate: '( 1 + 2' ].
    t expect: 'stray tokens' raises: ParseError in: [ c evaluate: '1 2 3 )' ].
    t expect: 'unterminated string' raises: ParseError in: [ c evaluate: '''oops' ].

    "----- error path: UndefinedVariableError (lowercase undefined name) -----"
    t expect: 'undefined var receiver' raises: UndefinedVariableError in: [ c evaluate: 'zzzUndefLower foo' ].
    t expect: 'undefined var bare' raises: UndefinedVariableError in: [ c evaluate: 'zzzUndefLower' ].
    t expect: 'undefined var assign' raises: UndefinedVariableError in: [ c evaluate: 'zzzUndefLower := 3' ].
    t expect: 'undefined superclass lower' raises: UndefinedVariableError in: [
        c buildClass: (Parser parseString: 'CmpErrSubA := zzzNoSuchSuper [ ]') parseClass ].
    t expect: 'undefined superclass upper' raises: UndefinedVariableError in: [
        c buildClass: (Parser parseString: 'CmpErrSubB := TotallyMissingUpperSuper [ ]') parseClass ].

    "----- error path: ReadonlyVariableError (pseudo vars and formal args) -----"
    t expect: 'assign self' raises: ReadonlyVariableError in: [ c evaluate: 'self := 3' ].
    t expect: 'assign true' raises: ReadonlyVariableError in: [ c evaluate: 'true := 3' ].
    t expect: 'assign nil' raises: ReadonlyVariableError in: [ c evaluate: 'nil := 3' ].
    t expect: 'assign super' raises: ReadonlyVariableError in: [ c evaluate: 'super := 3' ].
    t expect: 'assign method arg' raises: ReadonlyVariableError in: [
        c buildClass: (Parser parseString: 'CmpErrRo := Object [ foo: a [ a := 3 ] ]') parseClass ].

    "----- error path: RedefinitionError (duplicate temps/args/methods) -----"
    t expect: 'duplicate temp' raises: RedefinitionError in: [ c evaluate: '| a a | a' ].
    t expect: 'duplicate block arg' raises: RedefinitionError in: [ c evaluate: '[:x :x | x] value: 1 value: 2' ].
    t expect: 'duplicate method selector' raises: RedefinitionError in: [
        c buildClass: (Parser parseString: 'CmpErrDup := Object [ foo [ ^1 ] foo [ ^2 ] ]') parseClass ].

    "----- error path: InvalidPragmaError (bad <shape:> argument) -----"
    t expect: 'shape pragma string arg' raises: InvalidPragmaError in: [
        c buildClass: (Parser parseString: 'CmpErrShpA := Object [ <shape: ''hi''> ]') parseClass ].
    t expect: 'shape pragma char arg' raises: InvalidPragmaError in: [
        c buildClass: (Parser parseString: 'CmpErrShpB := Object [ <shape: $a> ]') parseClass ].

    "----- semantics contrast: an uppercase undefined global reads as nil, so the
           send is a RUNTIME MessageNotUnderstood, not a compile error -----"
    t expect: 'uppercase undefined -> nil DNU' raises: MessageNotUnderstood in: [
        c evaluate: 'UpperUndefGlobalXYZ zorkNotUnderstood' ].

    "----- error-class hierarchy: the compile errors subclass CompileError/Error,
           ParseError subclasses Error but not CompileError -----"
    t check: 'UndefinedVariableError isKindOf CompileError' that: [
        | ok | ok := false.
        [ c evaluate: 'zzzUndefLower foo' ] on: Exception do: [:e | ok := e isKindOf: CompileError ]. ok ].
    t check: 'ReadonlyVariableError isKindOf CompileError' that: [
        | ok | ok := false.
        [ c evaluate: 'self := 3' ] on: Exception do: [:e | ok := e isKindOf: CompileError ]. ok ].
    t check: 'RedefinitionError isKindOf Error' that: [
        | ok | ok := false.
        [ c evaluate: '| a a | a' ] on: Exception do: [:e | ok := e isKindOf: Error ]. ok ].
    t check: 'InvalidPragmaError isKindOf CompileError' that: [
        | ok | ok := false.
        [ c buildClass: (Parser parseString: 'CmpErrShpC := Object [ <shape: $b> ]') parseClass ]
            on: Exception do: [:e | ok := e isKindOf: CompileError ]. ok ].
    t check: 'ParseError isKindOf Error' that: [
        | ok | ok := false.
        [ c evaluate: '3 + ' ] on: Exception do: [:e | ok := e isKindOf: Error ]. ok ].
    t check: 'ParseError is not a CompileError' that: [
        | ok | ok := true.
        [ c evaluate: '3 + ' ] on: Exception do: [:e | ok := (e isKindOf: CompileError) not ]. ok ].

    "----- messageText content of the compile errors -----"
    t check: 'UndefinedVariableError names the variable' that: [
        | msg | msg := ''.
        [ c evaluate: 'zzzUndefLower foo' ] on: Exception do: [:e | msg := e messageText ].
        (msg indexOfSubCollection: 'Undefined variable' startingAt: 1) > 0 ].
    t check: 'ReadonlyVariableError mentions readonly' that: [
        | msg | msg := ''.
        [ c evaluate: 'self := 1' ] on: Exception do: [:e | msg := e messageText ].
        (msg indexOfSubCollection: 'readonly' startingAt: 1) > 0 ].
    t check: 'RedefinitionError mentions redefine' that: [
        | msg | msg := ''.
        [ c evaluate: '| a a | a' ] on: Exception do: [:e | msg := e messageText ].
        (msg indexOfSubCollection: 'redefine' startingAt: 1) > 0 ].
    t check: 'ParseError mentions parse error' that: [
        | msg | msg := ''.
        [ c evaluate: '3 + ' ] on: Exception do: [:e | msg := e messageText ].
        (msg indexOfSubCollection: 'Parse error' startingAt: 1) > 0 ].

    ^t report
]
