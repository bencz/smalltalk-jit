"Exercises the extended exception protocol: signal:, messageText, description,
return:, pass, ensure:, ifCurtailed: and retry. Split into small blocks to stay
under the compiler's large-method limit."

[
	"signalling and message text"
	Assert true: ([Error new signal: 'boom'] on: Error do: [:e | e messageText]) = 'boom'.
	Assert true: ([Error signal: 'klass'] on: Error do: [:e | e messageText]) = 'klass'.
	Assert true: ([Error new signal: 'd'] on: Error do: [:e | e description]) = 'd'.
	Assert true: ([Error signal] on: Error do: [:e | e class]) == Error.
	Assert true: ([42] on: Error do: [:e | 99]) = 42.
	Assert true: ([Error signal] on: Error do: [:e | 7]) = 7.
]


[
	"return: answers the value of the protected on:do:"
	Assert true: ([Error signal] on: Error do: [:e | e return: 123]) = 123.
	Assert true: ([Error signal] on: Error do: [:e | e return]) isNil.
	Assert true: (([Error signal: 'x'] on: Error do: [:e | e return: 5]) + 1) = 6.
]


[
	"pass re-signals to the enclosing handler"
	Assert true: ([[Error signal] on: Error do: [:e | e pass]] on: Error do: [:e | 'outer']) = 'outer'.
	Assert true: ([[Error new signal: 'orig'] on: Error do: [:e | e pass]]
		on: Error do: [:e | e messageText]) = 'orig'.
	"an unhandled inner class propagates to the matching outer handler"
	Assert true: ([[Error signal] on: MessageNotUnderstood do: [:e | 1]]
		on: Error do: [:e | 2]) = 2.
]


[
	"ensure: runs the cleanup on normal completion, answering the receiver value"
	| log |
	log := OrderedCollection new.
	Assert true: ([log add: 1. 42] ensure: [log add: 2]) = 42.
	Assert true: log first = 1.
	Assert true: log last = 2.

	"ensure: runs the cleanup while unwinding through an exception"
	log := OrderedCollection new.
	Assert true: ([[Error signal] ensure: [log add: #cleanup]]
		on: Error do: [:e | #caught]) = #caught.
	Assert true: (log includes: #cleanup).
]


[
	"ifCurtailed: runs only when curtailed"
	| log |
	log := OrderedCollection new.
	Assert true: ([100] ifCurtailed: [log add: #ran]) = 100.
	Assert true: log isEmpty.

	log := OrderedCollection new.
	([[Error signal] ifCurtailed: [log add: #ran]] on: Error do: [:e | nil]).
	Assert true: (log includes: #ran).
]


[
	"retry re-evaluates the protected block"
	| attempts result |
	attempts := OrderedCollection new.
	result := [attempts add: 1.
		attempts size < 3 ifTrue: [Error signal: 'again'].
		attempts size]
			on: Error do: [:e | e retry].
	Assert true: result = 3.
	Assert true: attempts size = 3.

	"resume is not supported and fails clearly rather than silently"
	Assert do: [[Error signal] on: Error do: [:e | e resume: 1]] expect: Error.
]
