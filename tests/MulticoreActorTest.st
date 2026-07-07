"Cross-isolate actors — ONE application spread across cores (no -i flag). The
 main isolate hosts a Coordinator actor; it spawns a WORKER isolate at runtime
 that defines + hosts a Counter actor, fires N #inc at the remote Counter, and
 verifies the exact count comes back. The actor model is entirely Smalltalk on
 the generic `Isolate spawn:` / send / receive substrate."

Coordinator := Actor [
	| expected doneSem |

	setExpected: n done: aSemaphore [ expected := n. doneSem := aSemaphore. ^self ]

	receiveMessage: ctx [
		"the Counter replies its final count once it has seen N increments"
		(ctx message isKindOf: Integer) ifTrue: [
			Assert true: ctx message = expected.
			Transcript nextPutAll: 'MULTICORE VERIFY count='; nextPutAll: ctx message printString;
				nextPutAll: ' expected='; nextPutAll: expected printString; lf.
			doneSem signal ].
		^self
	]
]

[
	| n coord worker doneSem |
	n := 1000.
	doneSem := Semaphore new.
	Actors bindToCurrentIsolate.
	coord := Actors spawn: (Props fromProducer: [ Coordinator new setExpected: n done: doneSem ]).

	"spawn a worker isolate on another core: it defines a Counter, hosts it as
	 actor id 1, goes multicore, and announces readiness back to us (isolate 0)."
	worker := Isolate spawn: '
		Counter := Actor [
			| count target |
			setTarget: k [ target := k. count := 0. ^self ]
			receiveMessage: ctx [
				ctx message = #inc ifTrue: [
					count := count + 1.
					count = target ifTrue: [ ctx respond: count ] ].
				^self ]
		]
		[ Actors goMulticore.
		  Actors spawn: (Props fromProducer: [ Counter new setTarget: 1000 ]).
		  [ Isolate send: #ready to: 0 ] whileFalse: [ (Delay forMilliseconds: 2) wait ] ]
	'.
	Assert true: worker > 0.

	"raw handshake: block until the worker says its Counter is up (so no #inc is
	 ever sent before the actor exists)."
	Isolate receive.
	Actors startReceptionist.

	"fire N increments at the remote Counter; on the Nth it replies its count."
	n timesRepeat: [ (Pid id: 1 isolate: worker) tell: #inc from: coord ].

	"wait for the Coordinator to verify, then shut both receptionists down so the
	 worker thread and this isolate can exit cleanly."
	doneSem wait.
	Isolate send: #stopReceptionist to: worker.
	Actors stopReceptionist.
]
