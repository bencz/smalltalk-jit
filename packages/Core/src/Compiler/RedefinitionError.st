RedefinitionError := CompileError [

	"accessing"

	defaultMessageText [
		"A node built by hand has no source position, and the name alone is
		 still the message."
		| sourceCode |

		sourceCode := identifier sourceCode.
		sourceCode isNil ifTrue: [ ^'Cannot redefine: ', identifier value ].
		^'Cannot redefine: ',
			identifier value,
			' in ''', sourceCode source,
			''' line ', sourceCode line printString,
			' column ', sourceCode column printString
	]

]
