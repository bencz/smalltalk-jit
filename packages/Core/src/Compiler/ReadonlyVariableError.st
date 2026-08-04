ReadonlyVariableError := CompileError [

	"accessing"

	defaultMessageText [
		"A node built by hand has no source position, and the name alone is
		 still the message."
		| sourceCode |

		sourceCode := identifier sourceCode.
		sourceCode isNil ifTrue: [
			^'Cannot write to readonly variable: ', identifier value ].
		^'Cannot write to readonly variable: ',
			identifier value,
			' in ''', sourceCode source,
			''' line ', sourceCode line printString,
			' column ', sourceCode column printString
	]

]
