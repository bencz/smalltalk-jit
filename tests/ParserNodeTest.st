"A visitor that tallies the nodes reached by acceptVisitor:, exercising the
 double-dispatch traversal (ParserTest.st only asserts isKindOf; this counts).
 It implements every visit selector the *Node acceptVisitor: methods call, so a
 missing one would surface as a MessageNotUnderstood inside a check block."
NodeCounter := Object [
    | total classN methodN blockN trueN falseN nilN charN arrN intN varN |
    reset [
        total := 0. classN := 0. methodN := 0. blockN := 0.
        trueN := 0. falseN := 0. nilN := 0. charN := 0. arrN := 0.
        intN := 0. varN := 0. ^self ]
    total [ ^total ]
    classN [ ^classN ]
    methodN [ ^methodN ]
    blockN [ ^blockN ]
    trueN [ ^trueN ]
    falseN [ ^falseN ]
    nilN [ ^nilN ]
    charN [ ^charN ]
    arrN [ ^arrN ]
    intN [ ^intN ]
    varN [ ^varN ]
    visitClassNode: aNode [ total := total + 1. classN := classN + 1 ]
    visitMethodNode: aNode [ total := total + 1. methodN := methodN + 1 ]
    visitBlockNode: aNode [ total := total + 1. blockN := blockN + 1 ]
    visitExpressionNode: aNode [ total := total + 1 ]
    visitMessageExpressionNode: aNode [ total := total + 1 ]
    visitVariableNode: aNode [ total := total + 1. varN := varN + 1 ]
    visitIntegerNode: aNode [ total := total + 1. intN := intN + 1 ]
    visitStringNode: aNode [ total := total + 1 ]
    visitSymbolNode: aNode [ total := total + 1 ]
    visitCharacterNode: aNode [ total := total + 1. charN := charN + 1 ]
    visitArrayNode: aNode [ total := total + 1. arrN := arrN + 1 ]
    visitTrueNode: aNode [ total := total + 1. trueN := trueN + 1 ]
    visitFalseNode: aNode [ total := total + 1. falseN := falseN + 1 ]
    visitNilNode: aNode [ total := total + 1. nilN := nilN + 1 ]
]

"=== ParserNodeTest ============================================================
 Direct AST coverage. Parser parseString: <src> then parseMethod / parseClass /
 parseMethodOrBlock IS reachable from Smalltalk and yields the node tree, so
 this file verifies node classes and their accessors, plus visitor traversal
 and a set of parse-error edge cases not covered by ParserTest.st.

 AST shape learned by probing:
   - LiteralNode subclasses (IntegerNode/StringNode/SymbolNode/CharacterNode/
     TrueNode/FalseNode/NilNode/ArrayNode/VariableNode) carry #value; VariableNode
     also answers #name.
   - ExpressionNode: #receiver #messageExpressions #assigments #returns.
   - A message CHAIN nests: 'a foo bar' has receiver = (a foo) ExpressionNode and
     one messageExpression 'bar'. A CASCADE 'a f; g; h' keeps one receiver and
     THREE messageExpressions.
   - MethodNode: #selector #arguments #temporaries #expressions #body(BlockNode).
   - ClassNode: #name #superName(VariableNode) #variables #methods.

 NOTE: the checks are spread over three top-level blocks that share one TestRun
 through the global PNRun. A single block holding all ~90 temp-capturing check
 closures miscompiles (see the report accompanying this file); keeping each
 block small sidesteps that codegen limit. The LAST block ends with ^PNRun
 report so its integer result is the process exit code.
 =============================================================================="

"----- Block 1: top-level node classes, MethodNode + ExpressionNode, literals -----"
[
    | p method expr |
    PNRun := TestRun named: 'ParserNode'.
    p := [ :s | Parser parseString: s ].

    PNRun check: 'parseMethod -> MethodNode' that: [ ((p value: 'foo [ ^42 ]') parseMethod) isKindOf: MethodNode ].
    PNRun check: 'parseClass -> ClassNode' that: [ ((p value: 'Foo := Object [ ]') parseClass) isKindOf: ClassNode ].
    PNRun check: 'parseMethodOrBlock method' that: [ ((p value: 'foo [ ]') parseMethodOrBlock) isKindOf: MethodNode ].
    PNRun check: 'parseMethodOrBlock block' that: [ ((p value: '[ 7 ]') parseMethodOrBlock) isKindOf: BlockNode ].

    method := (p value: 'foo [ ^42 ]') parseMethod.
    PNRun check: 'method selector' equals: 'foo' is: [ method selector ].
    PNRun check: 'method body is BlockNode' that: [ method body isKindOf: BlockNode ].
    PNRun check: 'method one expression' equals: 1 is: [ method expressions size ].
    PNRun check: 'method no arguments' equals: 0 is: [ method arguments size ].
    PNRun check: 'method no temporaries' equals: 0 is: [ method temporaries size ].

    expr := method expressions at: 1.
    PNRun check: 'expr is ExpressionNode' that: [ expr isKindOf: ExpressionNode ].
    PNRun check: 'expr returns true (had ^)' equals: true is: [ expr returns ].
    PNRun check: 'expr receiver IntegerNode' that: [ expr receiver isKindOf: IntegerNode ].
    PNRun check: 'IntegerNode value' equals: 42 is: [ expr receiver value ].

    PNRun check: 'no-caret last expr returns nil/false' that: [
        | m | m := (p value: 'foo [ 1. 2. 3 ]') parseMethod.
        (m expressions at: 3) returns ~= true ].

    method := (p value: 'bar: a and: b [ ^a ]') parseMethod.
    PNRun check: 'keyword selector' equals: 'bar:and:' is: [ method selector ].
    PNRun check: 'keyword two args' equals: 2 is: [ method arguments size ].
    PNRun check: 'keyword arg0 VariableNode' that: [ (method arguments at: 1) isKindOf: VariableNode ].
    PNRun check: 'keyword arg0 name' equals: 'a' is: [ (method arguments at: 1) name ].

    PNRun check: 'StringNode' that: [ ((p value: 'foo [ ''hi'' ]') parseMethod expressions at: 1) receiver isKindOf: StringNode ].
    PNRun check: 'StringNode value' equals: 'hi' is: [ ((p value: 'foo [ ''hi'' ]') parseMethod expressions at: 1) receiver value ].
    PNRun check: 'SymbolNode' that: [ ((p value: 'foo [ #sym ]') parseMethod expressions at: 1) receiver isKindOf: SymbolNode ].
    PNRun check: 'CharacterNode' that: [ ((p value: 'foo [ $a ]') parseMethod expressions at: 1) receiver isKindOf: CharacterNode ].
    PNRun check: 'CharacterNode value' equals: $a is: [ ((p value: 'foo [ $a ]') parseMethod expressions at: 1) receiver value ].
    PNRun check: 'TrueNode' that: [ ((p value: 'foo [ true ]') parseMethod expressions at: 1) receiver isKindOf: TrueNode ].
    PNRun check: 'FalseNode' that: [ ((p value: 'foo [ false ]') parseMethod expressions at: 1) receiver isKindOf: FalseNode ].
    PNRun check: 'NilNode' that: [ ((p value: 'foo [ nil ]') parseMethod expressions at: 1) receiver isKindOf: NilNode ].
    PNRun check: 'VariableNode' that: [ ((p value: 'foo [ Object ]') parseMethod expressions at: 1) receiver isKindOf: VariableNode ].
    PNRun check: 'VariableNode name' equals: 'Object' is: [ ((p value: 'foo [ Object ]') parseMethod expressions at: 1) receiver name ].
]

"----- Block 2: arrays, message structure, chains, cascades, blocks, assignment -----"
[
    | p expr msg arr block method |
    p := [ :s | Parser parseString: s ].

    arr := ((p value: 'foo [ #(1 $a ''s'' #sym) ]') parseMethod expressions at: 1) receiver.
    PNRun check: 'ArrayNode' that: [ arr isKindOf: ArrayNode ].
    PNRun check: 'ArrayNode size' equals: 4 is: [ arr value size ].
    PNRun check: 'array elem 1 IntegerNode' that: [ (arr value at: 1) isKindOf: IntegerNode ].
    PNRun check: 'array elem 1 value' equals: 1 is: [ (arr value at: 1) value ].
    PNRun check: 'array elem 2 CharacterNode' that: [ (arr value at: 2) isKindOf: CharacterNode ].
    PNRun check: 'array elem 3 StringNode' that: [ (arr value at: 3) isKindOf: StringNode ].
    PNRun check: 'array elem 4 SymbolNode' that: [ (arr value at: 4) isKindOf: SymbolNode ].

    expr := (p value: 'foo [ 3 + 4 ]') parseMethod expressions at: 1.
    PNRun check: 'binary receiver IntegerNode' that: [ expr receiver isKindOf: IntegerNode ].
    PNRun check: 'binary one message' equals: 1 is: [ expr messageExpressions size ].
    msg := expr messageExpressions at: 1.
    PNRun check: 'MessageExpressionNode' that: [ msg isKindOf: MessageExpressionNode ].
    PNRun check: 'binary selector' equals: '+' is: [ msg selector ].
    PNRun check: 'binary one argument' equals: 1 is: [ msg arguments size ].
    PNRun check: 'binary arg is IntegerNode' that: [ (msg arguments at: 1) isKindOf: IntegerNode ].
    PNRun check: 'binary arg value' equals: 4 is: [ (msg arguments at: 1) value ].

    expr := (p value: 'foo [ self at: 1 put: 2 ]') parseMethod expressions at: 1.
    PNRun check: 'keyword send selector' equals: 'at:put:' is: [ (expr messageExpressions at: 1) selector ].
    PNRun check: 'keyword send two args' equals: 2 is: [ (expr messageExpressions at: 1) arguments size ].
    PNRun check: 'keyword send arg1 value' equals: 1 is: [ ((expr messageExpressions at: 1) arguments at: 1) value ].

    expr := (p value: 'foo [ 3 factorial printString ]') parseMethod expressions at: 1.
    PNRun check: 'chain outer receiver is ExpressionNode' that: [ expr receiver isKindOf: ExpressionNode ].
    PNRun check: 'chain outer one message' equals: 1 is: [ expr messageExpressions size ].
    PNRun check: 'chain outer selector' equals: 'printString' is: [ (expr messageExpressions at: 1) selector ].
    PNRun check: 'chain inner receiver IntegerNode 3' equals: 3 is: [ expr receiver receiver value ].
    PNRun check: 'chain inner selector' equals: 'factorial' is: [ (expr receiver messageExpressions at: 1) selector ].

    expr := (p value: 'foo [ x add: 1; add: 2; add: 3 ]') parseMethod expressions at: 1.
    PNRun check: 'cascade receiver VariableNode x' equals: 'x' is: [ expr receiver name ].
    PNRun check: 'cascade three messages' equals: 3 is: [ expr messageExpressions size ].

    block := ((p value: 'foo [ [:a | a + 1] ]') parseMethod expressions at: 1) receiver.
    PNRun check: 'receiver is BlockNode' that: [ block isKindOf: BlockNode ].
    PNRun check: 'block one argument' equals: 1 is: [ block arguments size ].
    PNRun check: 'block arg VariableNode' that: [ (block arguments at: 1) isKindOf: VariableNode ].
    PNRun check: 'block one expression' equals: 1 is: [ block expressions size ].

    method := (p value: 'foo [ | x | x := 5 ]') parseMethod.
    PNRun check: 'method one temporary' equals: 1 is: [ method temporaries size ].
    PNRun check: 'temporary is VariableNode' that: [ (method temporaries at: 1) isKindOf: VariableNode ].
    expr := method expressions at: 1.
    PNRun check: 'assignment has one target' equals: 1 is: [ expr assigments size ].
    PNRun check: 'assignment target VariableNode' that: [ (expr assigments at: 1) isKindOf: VariableNode ].
    PNRun check: 'assignment target name' equals: 'x' is: [ (expr assigments at: 1) name ].
    PNRun check: 'assignment source value 5' equals: 5 is: [ expr receiver value ].
]

"----- Block 3: ClassNode, visitor traversal, parse-error edge cases (last block) -----"
[
    | p cls counter |
    p := [ :s | Parser parseString: s ].

    cls := (p value: 'Foo := Object [ | iv | bar [ ^1 ] ]') parseClass.
    PNRun check: 'class name VariableNode' that: [ cls name isKindOf: VariableNode ].
    PNRun check: 'class name value' equals: 'Foo' is: [ cls name value ].
    PNRun check: 'class superName VariableNode' that: [ cls superName isKindOf: VariableNode ].
    PNRun check: 'class superName value' equals: 'Object' is: [ cls superName value ].
    PNRun check: 'class one variable' equals: 1 is: [ cls variables size ].
    PNRun check: 'class variable VariableNode' that: [ (cls variables at: 1) isKindOf: VariableNode ].
    PNRun check: 'class one method' equals: 1 is: [ cls methods size ].
    PNRun check: 'class method0 MethodNode' that: [ (cls methods at: 1) isKindOf: MethodNode ].

    counter := NodeCounter new reset.
    (p value: 'CntCls := Object [ probe: a [ | b | b := 1 + 2. #(7 $z). true. false. nil ] ]')
        parseClass acceptVisitor: counter.
    PNRun check: 'visited exactly one ClassNode' equals: 1 is: [ counter classN ].
    PNRun check: 'visited exactly one MethodNode' equals: 1 is: [ counter methodN ].
    PNRun check: 'visited exactly one BlockNode' equals: 1 is: [ counter blockN ].
    PNRun check: 'visited exactly one TrueNode' equals: 1 is: [ counter trueN ].
    PNRun check: 'visited exactly one FalseNode' equals: 1 is: [ counter falseN ].
    PNRun check: 'visited exactly one NilNode' equals: 1 is: [ counter nilN ].
    PNRun check: 'visited exactly one CharacterNode' equals: 1 is: [ counter charN ].
    PNRun check: 'visited exactly one ArrayNode' equals: 1 is: [ counter arrN ].
    PNRun check: 'visited three IntegerNodes' equals: 3 is: [ counter intN ].
    PNRun check: 'visited several nodes overall' that: [ counter total > 10 ].

    PNRun expect: 'unterminated string' raises: ParseError in: [ (p value: 'foo [ ''unterminated ]') parseMethod ].
    PNRun expect: 'empty assignment rhs' raises: ParseError in: [ (p value: 'foo [ x := ]') parseMethod ].
    PNRun expect: 'unbalanced paren' raises: ParseError in: [ (p value: 'foo [ ( 1 + 2 ]') parseMethod ].
    PNRun expect: 'numeric method selector' raises: ParseError in: [ (p value: '123 [ ]') parseMethod ].
    PNRun expect: 'class without superclass' raises: ParseError in: [ (p value: 'Foo := [ ]') parseClass ].
    PNRun expect: 'class without body' raises: ParseError in: [ (p value: 'Foo := Object') parseClass ].
    PNRun check: 'ParseError isKindOf Error' that: [
        | ok | ok := false.
        [ (p value: 'foo [ ( 1 + 2 ]') parseMethod ] on: Exception do: [:e | ok := e isKindOf: Error ]. ok ].

    ^PNRun report
]
