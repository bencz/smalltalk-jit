[
	| compiler |

	CompilerRun := TestRun named: 'CompilerTest'.

	compiler := Compiler new.

	CompilerRun check: 'evaluate integer literal' that: [ (compiler evaluate: '1') == 1 ].
	CompilerRun check: 'evaluate explicit return' that: [ (compiler evaluate: '^1') == 1 ].
	CompilerRun check: 'evaluate self at top level is nil' that: [ (compiler evaluate: 'self') == nil ].
	CompilerRun check: 'evaluate temp declaration and assignment' that: [ (compiler evaluate: '| a | a := 1. a') == 1 ].
	CompilerRun expect: 'evaluate unknown message raises DNU' raises: MessageNotUnderstood in: [ compiler evaluate: 'self foo' ].
	CompilerRun check: 'Foo undefined before includeFile' that: [ Foo isNil ].
	compiler includeFile: 'tests/CompilerTestFile.st'.
	CompilerRun check: 'Foo defined after includeFile' that: [ Foo notNil ].
]

CompilerTestClassA := Object [

	| ClassVar |


	defineClassVar [
		ClassVar := 1
	]

]


CompilerTestClassB := CompilerTestClassA [

	classVar [
		^ClassVar
	]

]


[
	| object |

	object := CompilerTestClassB new.
	CompilerRun check: 'class var starts nil in subclass' that: [ object classVar isNil ].
	object defineClassVar.
	CompilerRun check: 'class var shared with subclass' equals: 1 is: [ object classVar ].
]

[ ^CompilerRun report ]
