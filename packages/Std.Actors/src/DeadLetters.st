"Where messages go when their target Pid has no live actor (stopped, never
 existed, or already terminated).

 It answers two questions the first version could not: WHAT was undeliverable,
 and HOW MANY have been. ProtoActor publishes a DeadLetterEvent on the event
 stream and lets subscribers (metrics, logging) decide what to do; there is no
 event stream here yet, so the two things a subscriber would have done -- count,
 and report -- are done directly. `count` is the part a test can assert on:
 before this, a dead letter was only observable as a request that timed out,
 which is also exactly what a slow actor looks like. `silent:` is how a test
 that EXPECTS a dead letter keeps it out of its own output."
DeadLetterProcess := Object [

	| count silent |


	class new [
		^super new initDeadLetters
	]


	initDeadLetters [
		count := 0.
		silent := false.
		^self
	]


	"observation"

	count [ ^count ]

	resetCount [ count := 0. ^self ]

	silent [ ^silent ]

	silent: aBoolean [ silent := aBoolean. ^self ]


	"the endpoint protocol, so the system can hand it a message exactly as it
	 hands one to a mailbox or a future"

	postUser: aMessage sender: aSenderPid [
		^self userMessage: aMessage to: nil from: aSenderPid
	]


	postSystem: aMessage [
		^self systemMessage: aMessage to: nil
	]


	"routing, with the target pid the endpoint protocol cannot carry"

	userMessage: aMessage to: aPid from: aSenderPid [
		count := count + 1.
		^self report: 'user' message: aMessage to: aPid
	]


	systemMessage: aMessage to: aPid [
		count := count + 1.
		^self report: 'system' message: aMessage to: aPid
	]


	report: aKind message: aMessage to: aPid [
		silent ifTrue: [ ^self ].
		Transcript nextPutAll: '[dead-letter] '; nextPutAll: aKind;
			nextPutAll: ' message '; nextPutAll: aMessage class name.
		aPid ifNotNil: [:p | Transcript nextPutAll: ' to '; nextPutAll: p printString ].
		Transcript nextPutAll: ' dropped'; lf.
		^self
	]
]
