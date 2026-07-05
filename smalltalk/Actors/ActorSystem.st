"The ActorSystem is the root of the actor world: it owns the process registry
 (Pid id -> endpoint), generates unique ids, routes messages, spawns top-level
 actors, and supervises them through a guardian strategy. A single default
 instance is published as the global `Actors` at bootstrap."
ActorSystem := Object [

	| registry deadLetters guardianStrategy counter |


	class initialize [
		Actors := self new.
	]


	class new [
		^super new initSystem
	]


	initSystem [
		registry := Dictionary new.
		counter := 0.
		guardianStrategy := OneForOneStrategy default.
		deadLetters := DeadLetterProcess new.
		^self
	]


	"------------------------------------------------------ registry"

	nextId [
		counter := counter + 1.
		^counter
	]


	registerEndpoint: anEndpoint [
		"Register any endpoint (mailbox, future, ...) and answer its Pid."
		| id pid |
		id := self nextId.
		pid := Pid system: self id: id.
		registry at: id put: anEndpoint.
		^pid
	]


	unregister: aPid [
		registry removeKey: aPid id ifAbsent: [].
		^self
	]


	endpointFor: aPid [
		^registry at: aPid id ifAbsent: [nil]
	]


	"------------------------------------------------------ delivery"

	deliverUser: aMessage to: aPid from: aSenderPid [
		| endpoint |
		endpoint := self endpointFor: aPid.
		endpoint isNil
			ifTrue: [ deadLetters postUser: aMessage sender: aSenderPid ]
			ifFalse: [ endpoint postUser: aMessage sender: aSenderPid ].
		^self
	]


	deliverSystem: aMessage to: aPid [
		| endpoint |
		endpoint := self endpointFor: aPid.
		endpoint ifNotNil: [:e | e postSystem: aMessage ].
		^self
	]


	"------------------------------------------------------ spawning"

	spawn: aProps [
		"Spawn a top-level actor supervised by the system guardian."
		^self spawnProps: aProps parent: nil
	]


	spawnProps: aProps parent: aParentPid [
		| id pid mailbox context |
		id := self nextId.
		pid := Pid system: self id: id.
		mailbox := Mailbox new.
		mailbox throughput: aProps throughput.
		context := ActorContext new.
		context system: self selfPid: pid parent: aParentPid props: aProps mailbox: mailbox.
		context actor: aProps producer value.
		mailbox invoker: context.
		registry at: id put: mailbox.
		mailbox postSystem: ActorStarted.
		mailbox start.
		^pid
	]


	"------------------------------------------------------ request / response"

	request: aMessage to: aPid [
		| future |
		future := Future in: self.
		aPid tell: aMessage from: future pid.
		^future
	]


	"------------------------------------------------------ guardian supervision"

	guardianHandleFailure: aFailure [
		"A top-level actor failed; the guardian strategy decides its fate."
		guardianStrategy handleFailure: aFailure context: self.
		^self
	]


	escalate: aFailure [
		"Nowhere left to escalate at the root: stop the offending actor."
		aFailure child sendSystem: SystemStop.
		^self
	]
]
