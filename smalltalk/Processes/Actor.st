"A minimal actor: an object with a private mailbox served by its own process.
 Messages are delivered asynchronously and handled one at a time, so an actor's
 instance state needs no locking. Subclass and add ordinary methods, then drive
 them with async:/async:with: and the actor performs them on its own process.
 See Processes/Actor demo programs for usage (Counter, Bank)."
Actor := Object [

	| mailbox running |


	class new [
		^super new initActor
	]


	initActor [
		mailbox := Channel new.
		running := false.
		^self
	]


	"lifecycle"

	start [
		"Spawn the actor's process; it serves the mailbox until stopped."
		running := true.
		[ [running] whileTrue: [ self dispatch: mailbox receive ] ] fork.
		^self
	]


	stop [
		"Ask the actor to finish after draining messages sent so far."
		mailbox send: (Array with: #stopActor).
		^self
	]


	stopActor [
		running := false.
	]


	"messaging"

	async: aSymbol [
		| message |
		message := Array new: 1.
		message at: 1 put: aSymbol.
		mailbox send: message.
		^self
	]


	async: aSymbol with: anArg [
		| message |
		message := Array new: 2.
		message at: 1 put: aSymbol.
		message at: 2 put: anArg.
		mailbox send: message.
		^self
	]


	async: aSymbol with: firstArg with: secondArg [
		| message |
		message := Array new: 3.
		message at: 1 put: aSymbol.
		message at: 2 put: firstArg.
		message at: 3 put: secondArg.
		mailbox send: message.
		^self
	]


	"dispatch (private)"

	dispatch: aMessage [
		| selector |
		selector := aMessage at: 1.
		aMessage size = 1
			ifTrue: [ self perform: selector ]
			ifFalse: [ self perform: selector withArguments: (aMessage copyFrom: 2 to: aMessage size) ].
	]

]
