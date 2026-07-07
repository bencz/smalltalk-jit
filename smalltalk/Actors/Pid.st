"A Pid is the only handle to an actor: an opaque address resolved through the
 ActorSystem's registry. Sending to a Pid never touches the actor directly, so a
 dead or restarted actor is handled transparently (dead letters).

 A Pid carries only { id . endpoint . isolate } — small, portable data. It does
 NOT store its ActorSystem: `system` always answers the current isolate's global
 `Actors`. That keeps a Pid trivially copyable across isolates (the message
 serializer just copies the three fields; `endpoint` is nil for registry pids),
 and a pid that arrives from another isolate automatically resolves `system` to
 the RECEIVING isolate's actor system. The `isolate` field says which isolate
 hosts the actor, so the system can route local vs. remote sends."
Pid := Object [

	| id endpoint isolate |


	class id: anId isolate: anIsolate [
		^self new setId: anId isolate: anIsolate
	]


	class system: anActorSystem id: anId [
		^self new setId: anId isolate: anActorSystem isolateId
	]


	class system: anActorSystem id: anId endpoint: anEndpoint [
		"A direct pid carries its endpoint, so delivery skips the registry.
		 Used for single-use Futures that never need dead-letter routing."
		^(self new setId: anId isolate: anActorSystem isolateId) setEndpoint: anEndpoint
	]


	setId: anId isolate: anIsolate [ id := anId. isolate := anIsolate. ^self ]

	setEndpoint: anEndpoint [ endpoint := anEndpoint. ^self ]


	"accessing"

	id [ ^id ]
	isolate [ ^isolate ]
	endpoint [ ^endpoint ]

	system [
		"The actor system is never stored on a pid (so pids stay portable across
		 isolates); it is always the current isolate's global."
		^Actors
	]


	"telling (fire and forget)"

	tell: aMessage [
		self system deliverUser: aMessage to: self from: nil.
		^self
	]


	tell: aMessage from: aSenderPid [
		self system deliverUser: aMessage to: self from: aSenderPid.
		^self
	]


	"request/response"

	request: aMessage [
		"Return a Future that completes when the actor responds."
		^self system request: aMessage to: self
	]


	request: aMessage timeout: milliseconds [
		^(self system request: aMessage to: self) awaitFor: milliseconds
	]


	"system control"

	sendSystem: aMessage [
		self system deliverSystem: aMessage to: self.
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
		^(aPid isKindOf: Pid)
			and: [ (id = aPid id) and: [ isolate = aPid isolate ] ]
	]

	hash [ ^id hash bitXor: isolate hash ]

	printOn: aStream [
		aStream nextPutAll: 'pid#'.
		id printOn: aStream.
		aStream nextPutAll: '@'.
		isolate printOn: aStream.
	]
]
