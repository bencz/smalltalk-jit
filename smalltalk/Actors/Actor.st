"Base class for actors. Subclasses implement domain behaviour by overriding
 `receiveMessage:` (the common case) and, if needed, the lifecycle hooks
 `started:`/`stopping:`/`stopped:`/`restarting:`. Advanced users may override
 `receive:` directly and pattern-match on `aContext message`.

 An actor instance is single-threaded by construction: only its own fiber ever
 runs its methods, so instance variables need no locking."
Actor := Object [

	"Single entry point called by the context for every message, including
	 lifecycle events. Default routes lifecycle to hooks and everything else to
	 receiveMessage:."
	receive: aContext [
		| msg |
		msg := aContext message.
		msg == ActorStarted    ifTrue: [ ^self started: aContext ].
		msg == ActorStopping   ifTrue: [ ^self stopping: aContext ].
		msg == ActorStopped    ifTrue: [ ^self stopped: aContext ].
		msg == ActorRestarting ifTrue: [ ^self restarting: aContext ].
		^self receiveMessage: aContext
	]


	"override this for ordinary messages"
	receiveMessage: aContext [
		"default: ignore"
		^self
	]


	"lifecycle hooks (override as needed)"
	started: aContext [ ^self ]
	stopping: aContext [ ^self ]
	stopped: aContext [ ^self ]
	restarting: aContext [ ^self ]
]
