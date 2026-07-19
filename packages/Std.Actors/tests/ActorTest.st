"Tests for the actor system, driven by the kernel TestRun. Assertions run on
 the main process only: an assertion inside an actor would be caught by
 supervision and would NOT fail the test, so assertions never live inside
 receive:. The scheduler runs one worker OS thread per core, so actors run in
 REAL parallelism: lifecycle events travel over a monitor-protected Channel
 (never a plain shared collection mutated across workers), and every wait for
 an asynchronous effect is a request/await round-trip or a bounded poll with
 Delay, never a counted-yield join. A supervised actor crash is caught by the
 supervisor and does not count as an uncaught error."

[ ActorRun := TestRun named: 'Actor' ]

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

"Parent/child pair reporting lifecycle events over a Channel."
CascadeParent := Actor [
	| events |
	class events: aChannel [ ^self new setEvents: aChannel ]
	setEvents: aChannel [ events := aChannel. ^self ]
	started: ctx [ ctx spawn: (Props fromProducer: [ CascadeChild events: events ]) ]
	receiveMessage: ctx [ ]
]
CascadeChild := Actor [
	| events |
	class events: aChannel [ ^self new setEvents: aChannel ]
	setEvents: aChannel [ events := aChannel. ^self ]
	started: ctx [ events send: #childStarted ]
	stopped: ctx [ events send: #childStopped ]
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
	ActorRun check: 'spawn answers a Pid' that: [ a isKindOf: Pid ].
	a tell: #add. a tell: #add. a tell: #add.
	"the mailbox is FIFO per sender, so #get is processed after the three #add"
	ActorRun check: 'three tells accumulate' equals: 3 is: [ (a request: #get) await ].
	a stop.
]

"request/await returns the actor's response; the actor sees the future as sender"
[
	| a replySender |
	a := Actors spawn: (Props fromActorClass: Accumulator).
	ActorRun check: 'fresh accumulator answers zero' equals: 0 is: [ (a request: #get) await ].
	replySender := (a request: #echo) await.
	ActorRun check: 'actor sees the future as sender' that: [ replySender isKindOf: Pid ].
	a stop.
]

"awaitFor: on a stopped actor times out (dead letter), answering nil. Stop is
 asynchronous under the pool, so poll (bounded) until the request dead-letters
 instead of assuming one yield is enough."
[
	| a tries r |
	a := Actors spawn: (Props fromActorClass: Accumulator).
	a stop.
	tries := 0.
	r := (a request: #get) awaitFor: 50.
	[ r isNil or: [ tries > 200 ] ] whileFalse: [
		tries := tries + 1.
		(Delay forMilliseconds: 5) wait.
		r := (a request: #get) awaitFor: 50 ].
	ActorRun check: 'request to stopped actor answers nil' that: [ r isNil ].
]


"--- hierarchy + stop cascade --------------------------------------------"

[
	| events parent tries evt |
	events := Channel new.
	parent := Actors spawn: (Props fromProducer: [ CascadeParent events: events ]).
	tries := 0.
	[ events isEmpty not or: [ tries > 400 ] ] whileFalse: [
		tries := tries + 1. (Delay forMilliseconds: 5) wait ].
	evt := events isEmpty ifTrue: [ #timedOut ] ifFalse: [ events receive ].
	ActorRun check: 'spawn cascade starts the child' equals: #childStarted is: [ evt ].
	parent stop.
	tries := 0.
	[ events isEmpty not or: [ tries > 400 ] ] whileFalse: [
		tries := tries + 1. (Delay forMilliseconds: 5) wait ].
	evt := events isEmpty ifTrue: [ #timedOut ] ifFalse: [ events receive ].
	ActorRun check: 'stop cascades to the child' equals: #childStopped is: [ evt ].
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
	ActorRun check: 'restart resets actor state' equals: 1 is: [ (child request: #get) await ].
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
	ActorRun check: 'resume keeps actor state' equals: 3 is: [ (child request: #get) await ].
	parent stop.
]

"stop decider: the child is gone after the crash; poll (bounded) until its
 requests dead-letter to nil"
[
	| parent child tries r |
	parent := Actors spawn: ((Props fromProducer: [
		SupervisingParent producing: (Props fromActorClass: SupCounter) ])
			withSupervisor: (OneForOneStrategy decider: [:reason | #stop])).
	child := (parent request: #child) await.
	child tell: #bump.
	child tell: #crash.                 "stop -> child is gone"
	tries := 0.
	r := (child request: #get) awaitFor: 50.
	[ r isNil or: [ tries > 200 ] ] whileFalse: [
		tries := tries + 1.
		(Delay forMilliseconds: 5) wait.
		r := (child request: #get) awaitFor: 50 ].
	ActorRun check: 'stop decider kills the child' that: [ r isNil ].
	parent stop.
]


"--- poison pill: processed after already-queued messages ----------------"

[
	| events a tries evt |
	events := Channel new.
	a := Actors spawn: (Props fromProducer: [ CascadeChild events: events ]).
	tries := 0.
	[ events isEmpty not or: [ tries > 400 ] ] whileFalse: [
		tries := tries + 1. (Delay forMilliseconds: 5) wait ].
	evt := events isEmpty ifTrue: [ #timedOut ] ifFalse: [ events receive ].
	ActorRun check: 'poisoned child had started' equals: #childStarted is: [ evt ].
	a poison.
	tries := 0.
	[ events isEmpty not or: [ tries > 400 ] ] whileFalse: [
		tries := tries + 1. (Delay forMilliseconds: 5) wait ].
	evt := events isEmpty ifTrue: [ #timedOut ] ifFalse: [ events receive ].
	ActorRun check: 'poison stops the child' equals: #childStopped is: [ evt ].
	ActorRun check: 'request after poison answers nil' that: [
		((a request: #get) awaitFor: 50) isNil ].
]

[ ^ActorRun report ]
