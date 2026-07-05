"A Pid is the only handle to an actor: an opaque address resolved through the
 ActorSystem's registry. Sending to a Pid never touches the actor directly, so
 a dead or restarted actor is handled transparently (dead letters)."
Pid := Object [

	| system id |


	class system: anActorSystem id: anId [
		^self new setSystem: anActorSystem id: anId
	]


	setSystem: anActorSystem id: anId [
		system := anActorSystem.
		id := anId.
		^self
	]


	"accessing"

	id [ ^id ]
	system [ ^system ]


	"telling (fire and forget)"

	tell: aMessage [
		system deliverUser: aMessage to: self from: nil.
		^self
	]


	tell: aMessage from: aSenderPid [
		system deliverUser: aMessage to: self from: aSenderPid.
		^self
	]


	"request/response"

	request: aMessage [
		"Return a Future that completes when the actor responds."
		^system request: aMessage to: self
	]


	request: aMessage timeout: milliseconds [
		^(system request: aMessage to: self) awaitFor: milliseconds
	]


	"system control"

	sendSystem: aMessage [
		system deliverSystem: aMessage to: self.
		^self
	]


	stop [
		self sendSystem: SystemStop.
		^self
	]


	poison [
		"Graceful stop: processed after messages already in the mailbox."
		self tell: PoisonPill.
		^self
	]


	"identity"

	= aPid [
		^(aPid isKindOf: Pid) and: [id = aPid id]
	]

	hash [ ^id hash ]

	printOn: aStream [
		aStream nextPutAll: 'pid#'.
		id printOn: aStream.
	]
]
