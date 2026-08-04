UndefinedVariableError := CompileError [

	"accessing"

	defaultMessageText [
		"A node built by hand (Compiler evaluate:withArguments: makes bare
		 VariableNodes) has no source position, and the name alone is still
		 the message."
		| sourceCode |

		sourceCode := identifier sourceCode.
		sourceCode isNil ifTrue: [ ^'Undefined variable: ', identifier value ].
		^'Undefined variable: ',
			identifier value,
			' in ''', sourceCode source,
			''' line ', sourceCode line printString,
			' column ', sourceCode column printString
	]

]
