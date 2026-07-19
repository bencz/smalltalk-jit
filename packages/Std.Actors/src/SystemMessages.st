"Lifecycle and control messages for the actor kernel.

 Lifecycle/control values are unique singletons (compared with ==):
   ActorStarted ActorStopping ActorStopped ActorRestarting
   PoisonPill SystemStop SystemRestart SystemResume
 Data-carrying system messages are instances of ActorFailure / ActorTerminated.
 User messages travel wrapped in an Envelope so the receiver knows the sender."

ActorLifecycle := Object [

	| name |

	class named: aSymbol [
		^self new setName: aSymbol
	]

	setName: aSymbol [
		name := aSymbol.
		^self
	]

	name [ ^name ]

	printOn: aStream [
		aStream nextPutAll: 'ActorLifecycle('.
		aStream nextPutAll: name.
		aStream nextPut: $).
	]
]


Envelope := Object [

	| message sender |

	class message: aMessage sender: aPid [
		^self new setMessage: aMessage sender: aPid
	]

	setMessage: aMessage sender: aPid [
		message := aMessage.
		sender := aPid.
		^self
	]

	message [ ^message ]
	sender [ ^sender ]
]


ActorFailure := Object [
	"Sent to a supervisor when one of its children fails while handling a message."

	| child reason failedMessage |

	class child: aPid reason: anException message: aMessage [
		^self new setChild: aPid reason: anException message: aMessage
	]

	setChild: aPid reason: anException message: aMessage [
		child := aPid.
		reason := anException.
		failedMessage := aMessage.
		^self
	]

	child [ ^child ]
	reason [ ^reason ]
	failedMessage [ ^failedMessage ]
]


ActorTerminated := Object [
	"Delivered to a parent when a child has fully stopped."

	| who |

	class who: aPid [
		^self new setWho: aPid
	]

	setWho: aPid [
		who := aPid.
		^self
	]

	who [ ^who ]
]


"Creates the lifecycle/control singletons as globals, once, at bootstrap."
ActorMessages := Object [

	class initialize [
		ActorStarted    := ActorLifecycle named: 'started'.
		ActorStopping   := ActorLifecycle named: 'stopping'.
		ActorStopped    := ActorLifecycle named: 'stopped'.
		ActorRestarting := ActorLifecycle named: 'restarting'.
		PoisonPill      := ActorLifecycle named: 'poisonPill'.
		SystemStop      := ActorLifecycle named: 'stop'.
		SystemRestart   := ActorLifecycle named: 'restart'.
		SystemResume    := ActorLifecycle named: 'resume'.
	]
]
