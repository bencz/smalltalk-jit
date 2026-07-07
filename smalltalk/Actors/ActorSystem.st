"The ActorSystem is the root of the actor world: it owns the process registry
 (Pid id -> endpoint), generates unique ids, routes messages, spawns top-level
 actors, and supervises them through a guardian strategy. A single default
 instance is published as the global `Actors` at bootstrap."
ActorSystem := Object [

	| registry deadLetters guardianStrategy counter isolateId receptionistRunning |


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


	"------------------------------------------------------ isolate identity"

	primIsolateId [
		"Id of the isolate (independent VM / OS thread) running this system.
		 Always 0 in single-isolate mode."
		<primitive: IsolateCurrentIdPrimitive>
		^0
	]


	isolateId [
		"Cached: the value is fixed for the lifetime of an isolate. The snapshot
		 is created before any isolate exists, so this is computed per isolate on
		 first use (and re-bound explicitly at actor-hosting-isolate boot)."
		isolateId isNil ifTrue: [ isolateId := self primIsolateId ].
		^isolateId
	]


	bindToCurrentIsolate [
		"Force (re)binding to the OS thread now running this system — called when
		 an isolate boots so a snapshot-shared ActorSystem adopts its real id."
		isolateId := self primIsolateId.
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
		"If the target lives on another isolate, ship the message there and stop."
		aPid isolate = self isolateId ifFalse: [
			^self sendRemote: aMessage to: aPid from: aSenderPid ].
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


	"------------------------------------------------------ cross-isolate

	 Multicore actors are pure Smalltalk on top of the isolate substrate
	 (Isolate send:to: / receive). A remote send serialises a small envelope
	 { targetId . senderId . senderIsolate . message } and copies it into the
	 target isolate's inbox; a per-isolate RECEPTIONIST green-thread drains the
	 inbox and re-delivers each envelope locally. The VM knows nothing of this —
	 it only moves byte-copies of objects between isolates."

	goMulticore [
		"Make this actor system reachable from peer isolates: adopt this OS
		 thread's isolate id and start draining remote messages. Call once, early,
		 on every isolate that hosts actors other isolates will message."
		self bindToCurrentIsolate.
		self startReceptionist.
		^self
	]


	sendRemote: aMessage to: aPid from: aSenderPid [
		"Route a user message to an actor on another isolate by copying an
		 envelope into that isolate's inbox. Fire-and-forget: if the message graph
		 is not portable (contains a block) the send is silently dropped."
		| senderId senderIsolate |
		aSenderPid isNil
			ifTrue: [ senderId := 0. senderIsolate := 0 ]
			ifFalse: [ senderId := aSenderPid id. senderIsolate := aSenderPid isolate ].
		Isolate
			send: (Array with: aPid id with: senderId with: senderIsolate with: aMessage)
			to: aPid isolate.
		^self
	]


	startReceptionist [
		"One green-thread per isolate drains the inbox and delivers envelopes."
		receptionistRunning == true ifTrue: [ ^self ].
		receptionistRunning := true.
		[ self receptionLoop ] fork.
		^self
	]


	receptionLoop [
		[ receptionistRunning ] whileTrue: [
			| envelope |
			envelope := Isolate receive.
			envelope == #stopReceptionist
				ifTrue: [ receptionistRunning := false ]
				ifFalse: [ self deliverEnvelope: envelope ] ].
		^self
	]


	deliverEnvelope: anEnvelope [
		"anEnvelope = { targetId . senderId . senderIsolate . message }, a private
		 copy in this isolate's heap. Rebuild local pids and deliver."
		| targetPid senderPid |
		targetPid := Pid id: (anEnvelope at: 1) isolate: self isolateId.
		senderPid := (anEnvelope at: 2) = 0
			ifTrue: [ nil ]
			ifFalse: [ Pid id: (anEnvelope at: 2) isolate: (anEnvelope at: 3) ].
		self deliverUser: (anEnvelope at: 4) to: targetPid from: senderPid.
		^self
	]


	stopReceptionist [
		"Ask this isolate's own receptionist to finish, so the isolate can wind
		 down once its actors are idle. Delivered through the inbox so it unblocks
		 the receptionist's `Isolate receive`."
		Isolate send: #stopReceptionist to: self isolateId.
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
