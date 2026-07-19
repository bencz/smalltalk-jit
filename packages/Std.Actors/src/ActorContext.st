"The execution context of a single actor: it owns the actor instance and its
 mailbox, tracks parent/children, exposes the messaging API to the actor, and
 turns mailbox invocations into calls on the actor while enforcing the lifecycle
 and supervision rules. An uncaught error in the actor is caught HERE, turned
 into a Failure to the supervisor, and never escapes to crash the fiber/VM."
ActorContext := Object [

	| system selfPid parent props actor mailbox
	  message sender children behaviors stopping |


	class new [
		^super new initContext
	]


	initContext [
		children := OrderedCollection new.
		behaviors := OrderedCollection new.
		stopping := false.
		^self
	]


	system: aSystem selfPid: aPid parent: aParent props: aProps mailbox: aMailbox [
		system := aSystem.
		selfPid := aPid.
		parent := aParent.
		props := aProps.
		mailbox := aMailbox.
		^self
	]


	actor: anActor [ actor := anActor. ^self ]
	mailbox [ ^mailbox ]


	"------------------------------------------------------ actor-facing API"

	message [ ^message ]
	sender [ ^sender ]
	pid [ ^selfPid ]
	myself [ ^selfPid ]
	parent [ ^parent ]
	children [ ^children ]
	system [ ^system ]


	send: aMessage to: aPid [
		"Fire-and-forget; the receiver sees us as the sender."
		aPid tell: aMessage from: selfPid.
		^self
	]


	request: aMessage to: aPid [
		"Return a Future for the reply."
		^system request: aMessage to: aPid
	]


	respond: aMessage [
		"Reply to whoever sent the message currently being handled."
		sender ifNotNil: [:s | s tell: aMessage from: selfPid ].
		^self
	]


	forward: aPid [
		"Pass the current message on, preserving the original sender."
		aPid tell: message from: sender.
		^self
	]


	"spawning children"

	spawn: aProps [
		| child |
		child := system spawnProps: aProps parent: selfPid.
		children add: child.
		^child
	]


	stop: aPid [ aPid sendSystem: SystemStop. ^self ]
	stopSelf [ selfPid sendSystem: SystemStop. ^self ]
	poison: aPid [ aPid poison. ^self ]


	"behaviours (become / unbecome)"

	become: aBlock [
		"Replace the current message handler with aBlock (takes the context)."
		behaviors isEmpty
			ifTrue: [ behaviors addLast: aBlock ]
			ifFalse: [ behaviors at: behaviors size put: aBlock ].
		^self
	]

	pushBehavior: aBlock [ behaviors addLast: aBlock. ^self ]

	unbecome [
		behaviors isEmpty ifFalse: [ behaviors removeLast ].
		^self
	]


	"------------------------------------------------------ mailbox invocation"

	invokeUser: anEnvelope [
		anEnvelope message == PoisonPill
			ifTrue: [ ^self handleStop ]
			ifFalse: [ self deliver: anEnvelope message from: anEnvelope sender ].
		^self
	]


	invokeSystem: aMessage [
		aMessage == ActorStarted   ifTrue: [ ^self deliver: ActorStarted from: nil ].
		aMessage == SystemStop     ifTrue: [ ^self handleStop ].
		aMessage == SystemRestart  ifTrue: [ ^self handleRestart ].
		aMessage == SystemResume   ifTrue: [ ^self handleResume ].
		(aMessage isKindOf: ActorFailure)    ifTrue: [ ^self handleChildFailure: aMessage ].
		(aMessage isKindOf: ActorTerminated) ifTrue: [ ^self handleChildTerminated: aMessage ].
		^self
	]


	"------------------------------------------------------ dispatch to actor"

	deliver: aMessage from: aSenderPid [
		"Run the actor's receive for one message, catching any error and
		 converting it into a supervision failure. THIS is the isolation point."
		message := aMessage.
		sender := aSenderPid.
		[ self runReceive ]
			on: Error
			do: [:error | self failWith: error ].
		^self
	]


	runReceive [
		behaviors isEmpty
			ifTrue: [ actor receive: self ]
			ifFalse: [ (behaviors at: behaviors size) value: self ].
		^self
	]


	failWith: anError [
		"Suspend the mailbox and escalate to the supervisor (parent, or the
		 system guardian for a top-level actor)."
		mailbox suspend.
		parent isNil
			ifTrue: [ system guardianHandleFailure:
				(ActorFailure child: selfPid reason: anError message: message) ]
			ifFalse: [ parent sendSystem:
				(ActorFailure child: selfPid reason: anError message: message) ].
		^self
	]


	"------------------------------------------------------ lifecycle"

	handleStop [
		stopping ifTrue: [ ^self ].
		stopping := true.
		self deliver: ActorStopping from: nil.
		children isEmpty
			ifTrue: [ self finishStop ]
			ifFalse: [ children do: [:child | child sendSystem: SystemStop] ].
		^self
	]


	finishStop [
		self deliver: ActorStopped from: nil.
		system unregister: selfPid.
		mailbox stopRunner.
		parent ifNotNil: [:p | p sendSystem: (ActorTerminated who: selfPid) ].
		^self
	]


	handleRestart [
		"Recreate the actor from Props (state reset), keep the mailbox & pid."
		self deliver: ActorRestarting from: nil.
		children do: [:child | child sendSystem: SystemStop].
		children := OrderedCollection new.
		behaviors := OrderedCollection new.
		actor := props producer value.
		mailbox resume.
		self deliver: ActorStarted from: nil.
		^self
	]


	handleResume [
		"Ignore the failure and keep the same actor instance."
		mailbox resume.
		^self
	]


	"------------------------------------------------------ supervision (parent side)"

	handleChildFailure: aFailure [
		props supervisor handleFailure: aFailure context: self.
		^self
	]


	handleChildTerminated: aTerminated [
		children remove: aTerminated who ifAbsent: [].
		self deliver: aTerminated from: nil.
		(stopping and: [children isEmpty]) ifTrue: [ self finishStop ].
		^self
	]


	escalate: aFailure [
		"Directive #escalate: propagate the child's failure as our own."
		self failWith: aFailure reason.
		^self
	]
]
