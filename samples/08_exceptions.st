"============================================================
  08 - Exceptions
  ------------------------------------------------------------
  Exception handling with on:do:, ensure:, return:, retry and
  user-defined exception classes.
  ============================================================"

"A custom exception is just a subclass of Error."
NegativeInput := Error [
	messageText [ ^'input must be non-negative' ]
]

"---- Basic catch: on:do: ----"
[
	| result |
	result := [10 / 0] on: Error do: [:e | 'caught: ' , e messageText].
	Transcript nextPutAll: 'divide by zero -> '; nextPutAll: result; lf.
]

"---- Signalling and handling a custom exception ----"
[
	| safeSqrt |
	safeSqrt := [:n |
		n < 0 ifTrue: [NegativeInput signal].
		n sqrt].

	Transcript nextPutAll: 'sqrt 16 = '.
	([safeSqrt value: 16] on: NegativeInput do: [:e | e messageText]) printNl.

	Transcript nextPutAll: 'sqrt -4 = '.
	([safeSqrt value: -4] on: NegativeInput do: [:e | e messageText]) printNl.
]

"---- return: supplies the value of the protected block ----"
[
	| value |
	value := [Error signal: 'boom'. 999]
		on: Error
		do: [:e | e return: -1].
	Transcript nextPutAll: 'return: gives '. value printNl.
]

"---- ensure: always runs (like 'finally') ----"
[
	| log |
	log := OrderedCollection new.
	[[log add: #work. Error signal: 'oops']
		ensure: [log add: #cleanup]]
			on: Error do: [:e | log add: #handled].
	Transcript nextPutAll: 'execution order = '. log printNl.
]

"---- retry: run the protected block again ----"
[
	| attempts result |
	attempts := 0.
	result := [
		attempts := attempts + 1.
		attempts < 3 ifTrue: [Error signal: 'not yet'].
		'succeeded on attempt ' , attempts printString]
			on: Error
			do: [:e | attempts < 3 ifTrue: [e retry] ifFalse: [e return: 'gave up']].
	Transcript nextPutAll: result; lf.
]
