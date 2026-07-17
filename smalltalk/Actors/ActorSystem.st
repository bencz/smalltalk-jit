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
		"registry is READ-MOSTLY: written on spawn/register/unregister, read on every
		 message delivery (endpointFor:). ConcurrentDictionary makes those reads
		 lock-free, so message delivery no longer serializes lookups on the global
		 sync monitor (the actor-endpoint multicore ceiling)."
		registry := ConcurrentDictionary new.
		counter := 0.
		guardianStrategy := OneForOneStrategy default.
		deadLetters := DeadLetterProcess new.
		^self
	]


	"------------------------------------------------------ isolate identity"

	isolateId [
		"Share-nothing isolates were removed; the VM is single shared-memory now,
		 so there is one isolate. Kept (always 0) because a Pid still records it."
		^0
	]


	"------------------------------------------------------ registry"

	nextId [
		"Unique id generation. Under the multi-worker pool `counter := counter + 1` is a
		 racy read-modify-write (→ duplicate Pids), so it runs under the global sync monitor."
		| id |
		Processor monitorEnter.
		counter := counter + 1.
		id := counter.
		Processor monitorExit.
		^id
	]


	registerEndpoint: anEndpoint [
		"Register any endpoint (mailbox, future, ...) and answer its Pid."
		| id pid |
		id := self nextId.
		pid := Pid system: self id: id.
		registry at: id put: anEndpoint. "ConcurrentDictionary: COW write, self-synchronized"
		^pid
	]


	unregister: aPid [
		"COW write on the registry; ConcurrentDictionary serializes writers itself."
		registry removeKey: aPid id ifAbsent: [].
		^self
	]


	endpointFor: aPid [
		"LOCK-FREE read: the hot path of every message delivery. No monitor."
		^registry at: aPid id ifAbsent: [nil]
	]


	"------------------------------------------------------ delivery"

	deliverUser: aMessage to: aPid from: aSenderPid [
		| endpoint |
		"Direct pids (Futures) carry their endpoint — no registry lookup, no
		 dead-letter routing needed (they complete at most once)."
		endpoint := aPid endpoint.
		endpoint isNil ifTrue: [ endpoint := self endpointFor: aPid ].
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
		registry at: id put: mailbox. "ConcurrentDictionary: COW write, self-synchronized"
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
