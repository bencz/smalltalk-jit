[ ParserRun := TestRun named: 'ParserTest'. ]

MockNodeVisitor := Object [

	visitClassNode: aNode [
		ParserRun check: 'visitor gets ClassNode' that: [ aNode isKindOf: ClassNode ].
	]


	visitVariableNode: aNode [
		ParserRun check: 'visitor gets VariableNode' that: [ aNode isKindOf: VariableNode ].
	]


	visitMethodNode: aNode [
		ParserRun check: 'visitor gets MethodNode' that: [ aNode isKindOf: MethodNode ].
	]


	visitBlockNode: aNode [
		ParserRun check: 'visitor gets BlockNode' that: [ aNode isKindOf: BlockNode ].
	]


	visitExpressionNode: aNode [
		ParserRun check: 'visitor gets ExpressionNode' that: [ aNode isKindOf: ExpressionNode ].
	]


	visitIntegerNode: aNode [
		ParserRun check: 'visitor gets IntegerNode' that: [ aNode isKindOf: IntegerNode ].
	]


	visitMessageExpressionNode: aNode [
		ParserRun check: 'visitor gets MessageExpressionNode' that: [ aNode isKindOf: MessageExpressionNode ].
	]


	visitArrayNode: aNode [
		ParserRun check: 'visitor gets ArrayNode' that: [ aNode isKindOf: ArrayNode ].
	]


	visitCharacterNode: aNode [
		ParserRun check: 'visitor gets CharacterNode' that: [ aNode isKindOf: CharacterNode ].
	]


	visitFalseNode: aNode [
		ParserRun check: 'visitor gets FalseNode' that: [ aNode isKindOf: FalseNode ].
	]


	visitTrueNode: aNode [
		ParserRun check: 'visitor gets TrueNode' that: [ aNode isKindOf: TrueNode ].
	]


	visitNilNode: aNode [
		ParserRun check: 'visitor gets NilNode' that: [ aNode isKindOf: NilNode ].
	]

]


[ "test valid smalltalk code"
	| parser |

	parser := Parser parseString: 'Foo := Object []'.
	ParserRun check: 'parseClass answers ClassNode' that: [ parser parseClass isKindOf: ClassNode ].
	parser := Parser parseString: 'foo []'.
	ParserRun check: 'parseMethod answers MethodNode' that: [ parser parseMethod isKindOf: MethodNode ].
	parser := Parser parseString: 'foo []'.
	ParserRun check: 'parseMethodOrBlock answers MethodNode for method' that: [ parser parseMethodOrBlock isKindOf: MethodNode ].
	parser := Parser parseString: '[]'.
	ParserRun check: 'parseMethodOrBlock answers BlockNode for block' that: [ parser parseMethodOrBlock isKindOf: BlockNode ].
]

[ "test empty input"
	| parser |

	parser := Parser parseString: ''.
	ParserRun expect: 'parseClass on empty input' raises: ParseError in: [ parser parseClass ].
	parser := Parser parseString: ''.
	ParserRun expect: 'parseMethod on empty input' raises: ParseError in: [ parser parseMethod ].
	parser := Parser parseString: ''.
	ParserRun expect: 'parseMethodOrBlock on empty input' raises: ParseError in: [ parser parseMethodOrBlock ].
]

[ "test invalid smalltalk code"
	| parser |

	parser := Parser parseString: 'a'.
	ParserRun expect: 'parseClass on bare identifier' raises: ParseError in: [ parser parseClass ].
	parser := Parser parseString: 'Object subclass'.
	ParserRun expect: 'parseClass on unary subclass form' raises: ParseError in: [ parser parseClass ].
	parser := Parser parseString: 'Object subclass: Foo'.
	ParserRun expect: 'parseClass on keyword subclass form' raises: ParseError in: [ parser parseClass ].
	parser := Parser parseString: 'Object subclass: $'.
	ParserRun expect: 'parseClass on truncated character literal' raises: ParseError in: [ parser parseClass ].

	parser := Parser parseString: 'a'.
	ParserRun expect: 'parseMethod on bare identifier' raises: ParseError in: [ parser parseMethod ].
	parser := Parser parseString: 'a ['.
	ParserRun expect: 'parseMethod on unterminated unary method' raises: ParseError in: [ parser parseMethod ].
	parser := Parser parseString: 'a: ['.
	ParserRun expect: 'parseMethod on keyword selector missing arg' raises: ParseError in: [ parser parseMethod ].
	parser := Parser parseString: '+ 1 ['.
	ParserRun expect: 'parseMethod on binary selector with literal arg' raises: ParseError in: [ parser parseMethod ].

	parser := Parser parseString: 'a'.
	ParserRun expect: 'parseMethodOrBlock on bare identifier' raises: ParseError in: [ parser parseMethodOrBlock ].
	parser := Parser parseString: 'a ['.
	ParserRun expect: 'parseMethodOrBlock on unterminated method' raises: ParseError in: [ parser parseMethodOrBlock ].
	parser := Parser parseString: 'a: ['.
	ParserRun expect: 'parseMethodOrBlock on keyword missing arg' raises: ParseError in: [ parser parseMethodOrBlock ].
	parser := Parser parseString: '[x:]'.
	ParserRun expect: 'parseMethodOrBlock on block arg missing body' raises: ParseError in: [ parser parseMethodOrBlock ].
]

[ "extra long method selector"
	| stream method |

	stream := CollectionStream with: (String new: 512).
	stream next: 256 put: $a; nextPutAll: '[]'.

	method := (Parser parseString: stream contents) parseMethod.
	ParserRun check: 'long selector size' equals: 256 is: [ method selector size ].
	ParserRun check: 'long selector first char' equals: $a is: [ method selector at: 1 ].
	ParserRun check: 'long selector last char' equals: $a is: [ method selector at: 256 ].
]

[ "visitor test"
	| parser |

	parser := Parser parseString: 'Foo := Object [ bar: a [ | b | b := 1 + 2. #(1 $a). false. true. nil ] ]'.
	parser parseClass acceptVisitor: MockNodeVisitor new.
]

[ ^ParserRun report ]
