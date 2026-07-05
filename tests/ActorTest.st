"Tests for the actor system. Assertions run on the main process; actor state
 is inspected synchronously via request/await, and lifecycle events are observed
 through a shared log the actors append to (safe under the cooperative
 scheduler). An assertion inside an actor would be caught by supervision and
 would NOT fail the test, so assertions never live inside receive:."

"--- helper actors -------------------------------------------------------"

Accumulator := Actor [
	| total |
	started: ctx [ total := 0 ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		m = #add ifTrue: [ total := total + 1 ].
		m = #get ifTrue: [ ctx respond: total ].
		m = #echo ifTrue: [ ctx respond: ctx sender ].
	]
]

"Counter used for supervision tests; #crash raises."
SupCounter := Actor [
	| n |
	started: ctx [ n := 0 ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		m = #bump ifTrue: [ n := n + 1 ].
		m = #crash ifTrue: [ self error: 'boom' ].
		m = #get ifTrue: [ ctx respond: n ].
	]
]

"Parent/child pair recording lifecycle into a shared log."
CascadeParent := Actor [
	| log |
	class log: aLog [ ^self new setLog: aLog ]
	setLog: aLog [ log := aLog. ^self ]
	started: ctx [ ctx spawn: (Props fromProducer: [ CascadeChild log: log ]) ]
	receiveMessage: ctx [ ]
]
CascadeChild := Actor [
	| log |
	class log: aLog [ ^self new setLog: aLog ]
	setLog: aLog [ log := aLog. ^self ]
	started: ctx [ log add: #childStarted ]
	stopped: ctx [ log add: #childStopped ]
]

"A parent that hands its child's Pid back on request, so tests can message the
 child directly and exercise the parent's supervisor strategy."
SupervisingParent := Actor [
	| child childProps |
	class producing: aChildProps [ ^self new setChildProps: aChildProps ]
	setChildProps: aChildProps [ childProps := aChildProps. ^self ]
	started: ctx [ child := ctx spawn: childProps ]
	receiveMessage: ctx [ ctx message = #child ifTrue: [ ctx respond: child ] ]
]


"--- spawn / tell / request ----------------------------------------------"

[
	| a |
	a := Actors spawn: (Props fromActorClass: Accumulator).
	Assert true: (a isKindOf: Pid).
	a tell: #add. a tell: #add. a tell: #add.
	Assert true: (a request: #get) await = 3.
	a stop.
]

"request/await returns the actor's response; the actor sees the future as sender"
[
	| a replySender |
	a := Actors spawn: (Props fromActorClass: Accumulator).
	Assert true: (a request: #get) await = 0.
	replySender := (a request: #echo) await.
	Assert true: (replySender isKindOf: Pid).
	a stop.
]

"awaitFor: on a stopped actor times out (dead letter), answering nil"
[
	| a |
	a := Actors spawn: (Props fromActorClass: Accumulator).
	a stop.
	Processor yield.
	Assert true: ((a request: #get) awaitFor: 50) isNil.
]


"--- hierarchy + stop cascade --------------------------------------------"

[
	| log parent |
	log := OrderedCollection new.
	parent := Actors spawn: (Props fromProducer: [ CascadeParent log: log ]).
	1 to: 6 do: [:i | Processor yield].
	Assert true: (log includes: #childStarted).
	parent stop.
	1 to: 6 do: [:i | Processor yield].
	Assert true: (log includes: #childStopped).
]


"--- supervision: restart resets state, resume keeps it ------------------"

[
	| parent child |
	parent := Actors spawn: (Props fromProducer: [
		SupervisingParent producing: (Props fromActorClass: SupCounter) ]).
	child := (parent request: #child) await.
	child tell: #bump. child tell: #bump.
	child tell: #crash.                 "default guardian strategy = restart"
	child tell: #bump.
	Assert true: (child request: #get) await = 1.    "state reset by restart"
	parent stop.
]

[
	| parent child |
	parent := Actors spawn: ((Props fromProducer: [
		SupervisingParent producing: (Props fromActorClass: SupCounter) ])
			withSupervisor: (OneForOneStrategy decider: [:reason | #resume])).
	child := (parent request: #child) await.
	child tell: #bump. child tell: #bump.
	child tell: #crash.                 "resume -> same instance, state kept"
	child tell: #bump.
	Assert true: (child request: #get) await = 3.
	parent stop.
]

[
	| parent child |
	parent := Actors spawn: ((Props fromProducer: [
		SupervisingParent producing: (Props fromActorClass: SupCounter) ])
			withSupervisor: (OneForOneStrategy decider: [:reason | #stop])).
	child := (parent request: #child) await.
	child tell: #bump.
	child tell: #crash.                 "stop -> child is gone"
	Processor yield.
	Assert true: ((child request: #get) awaitFor: 50) isNil.
	parent stop.
]


"--- poison pill: processed after already-queued messages ----------------"

[
	| log a |
	log := OrderedCollection new.
	a := Actors spawn: (Props fromProducer: [ CascadeChild log: log ]).
	a poison.
	1 to: 6 do: [:i | Processor yield].
	Assert true: (log includes: #childStopped).
	Assert true: ((a request: #get) awaitFor: 50) isNil.
]
