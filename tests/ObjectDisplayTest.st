"ObjectDisplayTest: wave 3 Object display protocol. displayString (undecorated
 vs printString) for String, Symbol, Character and Integer; displayNl answering
 self; and Object>>assert: over both a Boolean and a Block."

[
	| t |
	t := TestRun named: 'ObjectDisplay'.

	"---- displayString vs printString ----"
	t check: 'String displayString is the raw content' equals: 'abc' is: [ 'abc' displayString ].
	t check: 'String printString keeps the quotes' equals: '''abc''' is: [ 'abc' printString ].
	t check: 'String displayString differs from printString' that: [
		'abc' displayString ~= 'abc' printString ].

	t check: 'Symbol displayString drops the #' equals: 'foo' is: [ #foo displayString ].
	t check: 'Symbol printString keeps the #' equals: '#foo' is: [ #foo printString ].

	t check: 'Character displayString drops the dollar' equals: 'A' is: [ $A displayString ].
	t check: 'Character printString keeps the dollar' equals: '$A' is: [ $A printString ].

	t check: 'Integer displayString equals printString' equals: '42' is: [ 42 displayString ].
	t check: 'Integer display and print agree' that: [ 42 displayString = 42 printString ].

	"---- displayNl answers self ----"
	t check: 'displayNl answers the receiver (Integer)' that: [ 42 displayNl == 42 ].
	t check: 'displayNl answers the receiver (String)' that: [ | s | s := 'hi'. s displayNl == s ].

	"---- assert: over a Boolean ----"
	t check: 'assert: true answers self' that: [ 7 assert: true. true ].
	t check: 'assert: true returns the receiver' that: [ (7 assert: true) == 7 ].
	t expect: 'assert: false raises AssertError' raises: AssertError in: [ 7 assert: false ].

	"---- assert: over a Block ----"
	t check: 'assert: a true block passes' that: [ 7 assert: [ 3 > 2 ]. true ].
	t expect: 'assert: a false block raises AssertError' raises: AssertError in: [ 7 assert: [ 3 < 2 ] ].

	^t report
]
