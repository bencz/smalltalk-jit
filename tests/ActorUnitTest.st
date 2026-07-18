"Unit-level, deterministic tests for the actor kernel: Pid, Props, Mailbox
 (standalone at the queue level, no runner fiber), ActorContext accessors and
 become/unbecome, and dead-letter routing. Assertions run on the main process;
 actor state is inspected synchronously via request/await, exactly as ActorTest
 does. This file deliberately avoids stress: every actor handles single-digit
 message counts. It does NOT duplicate ActorTest (spawn/tell/request/supervision/
 cascade/poison); it exercises the units and context API those tests skip."

"--- helper actors -------------------------------------------------------"

"Small probe exposing count plus several ActorContext accessors via respond:."
UnitProbe := Actor [
	| count |
	started: ctx [ count := 0 ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		m = #inc        ifTrue: [ count := count + 1 ].
		m = #count      ifTrue: [ ctx respond: count ].
		m = #whoami     ifTrue: [ ctx respond: ctx myself ].
		m = #parentNil  ifTrue: [ ctx respond: ctx parent isNil ].
		m = #childCount ifTrue: [ ctx respond: ctx children size ].
		m = #sys        ifTrue: [ ctx respond: (ctx system == Actors) ].
	]
]

"Probe for become:/unbecome. Default behavior answers #pong; after #switch it
 installs a block that answers #switched and reverts on #revert."
BecomeProbe := Actor [
	receiveMessage: ctx [
		| m |
		m := ctx message.
		m = #ping   ifTrue: [ ctx respond: #pong ].
		m = #switch ifTrue: [
			ctx become: [:c |
				c message = #revert
					ifTrue: [ c unbecome ]
					ifFalse: [ c respond: #switched ] ] ].
	]
]


"--- driver: ONE TestRun, its ^report becomes the exit code --------------"

[
	| t |
	t := TestRun named: 'ActorUnit'.

	"------------------------------------------------ Pid: identity & value"

	t check: 'pid = pid same id/isolate' that: [
		| a b | a := Pid id: 5 isolate: 0. b := Pid id: 5 isolate: 0. a = b ].
	t check: 'pid ~= pid different id' that: [
		| a b | a := Pid id: 5 isolate: 0. b := Pid id: 6 isolate: 0. (a = b) = false ].
	t check: 'pid ~= pid different isolate' that: [
		| a b | a := Pid id: 5 isolate: 0. b := Pid id: 5 isolate: 1. (a = b) = false ].
	t check: 'pid ~= non-pid' that: [
		| a | a := Pid id: 5 isolate: 0. (a = 5) = false ].
	t check: 'pid == itself' that: [
		| a | a := Pid id: 5 isolate: 0. a == a ].
	t check: 'distinct equal pids are not identical' that: [
		| a b | a := Pid id: 3 isolate: 0. b := Pid id: 3 isolate: 0.
		(a = b) and: [ (a == b) = false ] ].
	t check: 'equal pids share hash' that: [
		| a b | a := Pid id: 9 isolate: 0. b := Pid id: 9 isolate: 0. a hash = b hash ].
	t check: 'pid printString is pid#id@isolate' equals: 'pid#7@0' is: [
		| a | a := Pid id: 7 isolate: 0. a printString ].
	t check: 'pid system is the global Actors' that: [
		| a | a := Pid id: 1 isolate: 0. a system == Actors ].

	"------------------------------------------------ Props: build & config"

	t check: 'fromActorClass producer builds the actor' that: [
		| p | p := Props fromActorClass: UnitProbe. p producer value isKindOf: UnitProbe ].
	t check: 'fromActorClass default supervisor is OneForOne' that: [
		| p | p := Props fromActorClass: UnitProbe. p supervisor isKindOf: OneForOneStrategy ].
	t check: 'fromActorClass default throughput' equals: 30 is: [
		| p | p := Props fromActorClass: UnitProbe. p throughput ].
	t check: 'fromProducer producer builds the actor' that: [
		| p | p := Props fromProducer: [ UnitProbe new ]. p producer value isKindOf: UnitProbe ].
	t check: 'producer yields a fresh instance each call' that: [
		| p | p := Props fromActorClass: UnitProbe. (p producer value == p producer value) = false ].
	t check: 'withThroughput sets throughput' equals: 5 is: [
		| p | p := Props fromActorClass: UnitProbe. p withThroughput: 5. p throughput ].
	t check: 'withSupervisor sets strategy and returns self' that: [
		| p s r | p := Props fromActorClass: UnitProbe.
		s := OneForOneStrategy decider: [:reason | #stop].
		r := p withSupervisor: s.
		(r == p) and: [ p supervisor == s ] ].

	"------------------------------------------------ Mailbox: queue level"
	"Tested standalone WITHOUT start (no runner fiber), so the queue state is
	 exactly what we posted and takeNext:/hasProcessable are fully deterministic.
	 Mailbox has no isEmpty/size/peek selectors, so those are not tested."

	t check: 'empty mailbox has nothing processable' that: [
		| m | m := Mailbox new. m hasProcessable = false ].
	t check: 'mailbox processable after a user post' that: [
		| m | m := Mailbox new. m postUser: #z sender: nil. m hasProcessable ].
	t check: 'mailbox delivers user messages FIFO' that: [
		| m e1 e2 e3 | m := Mailbox new.
		m postUser: #a sender: nil. m postUser: #b sender: nil. m postUser: #c sender: nil.
		e1 := m takeNext: 0. e2 := m takeNext: 0. e3 := m takeNext: 0.
		(e1 message = #a) and: [ (e2 message = #b) and: [ e3 message = #c ] ] ].
	t check: 'drained mailbox yields nil' that: [
		| m | m := Mailbox new. m postUser: #a sender: nil.
		m takeNext: 0. (m takeNext: 0) isNil ].
	t check: 'system messages take priority over user' that: [
		| m first second | m := Mailbox new.
		m postUser: #u sender: nil. m postSystem: SystemStop.
		first := m takeNext: 0. second := m takeNext: 0.
		(first == SystemStop) and: [ second message = #u ] ].
	t check: 'throughput bound holds back user at the limit' that: [
		| m | m := Mailbox new. m postUser: #x sender: nil.
		(m takeNext: 30) isNil and: [ (m takeNext: 0) message = #x ] ].
	t check: 'suspend hides user, system still processable' that: [
		| m | m := Mailbox new. m suspend. m postSystem: SystemStop. m hasProcessable ].
	t check: 'suspend blocks a user take' that: [
		| m | m := Mailbox new. m postUser: #s sender: nil. m suspend. (m takeNext: 0) isNil ].
	t check: 'resume restores user delivery' that: [
		| m afterSuspend afterResume | m := Mailbox new.
		m postUser: #s sender: nil. m suspend.
		afterSuspend := m takeNext: 0.
		m resume.
		afterResume := m takeNext: 0.
		afterSuspend isNil and: [ afterResume message = #s ] ].

	"------------------------------------------ Actor lifecycle (tiny scale)"

	t check: 'spawn answers a Pid' that: [
		| a r | a := Actors spawn: (Props fromActorClass: UnitProbe).
		r := a isKindOf: Pid. a stop. r ].
	t check: 'tell mutates state, request reads it back' equals: 2 is: [
		| a got | a := Actors spawn: (Props fromActorClass: UnitProbe).
		a tell: #inc. a tell: #inc.
		got := (a request: #count) await. a stop. got ].
	t check: 'ctx myself is the spawn pid (equal and identical)' that: [
		| a who | a := Actors spawn: (Props fromActorClass: UnitProbe).
		who := (a request: #whoami) await. a stop.
		(who = a) and: [ who == a ] ].
	t check: 'top-level actor has a nil parent' that: [
		| a r | a := Actors spawn: (Props fromActorClass: UnitProbe).
		r := (a request: #parentNil) await. a stop. r ].
	t check: 'fresh actor has no children' equals: 0 is: [
		| a r | a := Actors spawn: (Props fromActorClass: UnitProbe).
		r := (a request: #childCount) await. a stop. r ].
	t check: 'ctx system resolves to the global Actors' that: [
		| a r | a := Actors spawn: (Props fromActorClass: UnitProbe).
		r := (a request: #sys) await. a stop. r ].
	t check: 'become: replaces the message handler' that: [
		| a before after | a := Actors spawn: (Props fromActorClass: BecomeProbe).
		before := (a request: #ping) await.
		a tell: #switch.
		after := (a request: #ping) await.
		a stop.
		(before = #pong) and: [ after = #switched ] ].
	t check: 'unbecome restores the prior handler' that: [
		| a r1 r2 r3 | a := Actors spawn: (Props fromActorClass: BecomeProbe).
		r1 := (a request: #ping) await.
		a tell: #switch.
		r2 := (a request: #ping) await.
		a tell: #revert.
		r3 := (a request: #ping) await.
		a stop.
		(r1 = #pong) and: [ (r2 = #switched) and: [ r3 = #pong ] ] ].

	"---------------------------------------------------------- dead letters"
	"A dead letter is not directly readable (DeadLetterProcess only prints), so
	 it is observed as ActorTest does: the reply never arrives and the request
	 future times out to nil. The '[dead-letter]' lines printed below are expected."

	t check: 'request to an unknown pid times out to nil' that: [
		| dead | dead := Pid id: 987654321 isolate: 0.
		((dead request: #anything) awaitFor: 50) isNil ].
	t check: 'tell to an unknown pid does not crash' that: [
		| dead | dead := Pid id: 987654322 isolate: 0. dead tell: #hi. true ].
	t check: 'request to a stopped actor times out to nil' that: [
		| a | a := Actors spawn: (Props fromActorClass: UnitProbe).
		a stop. Processor yield.
		((a request: #count) awaitFor: 50) isNil ].

	^t report
]
