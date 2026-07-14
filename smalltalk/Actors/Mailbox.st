"Each actor has one Mailbox, served by its own fiber (the runner). Messages
 are split into a high-priority system queue and a user queue. The runner drains
 all system messages, then up to `throughput` user messages, then yields for
 fairness; when nothing is processable it parks and is resumed on the next post.

 On failure the mailbox is SUSPENDED: system messages (supervisor directives)
 keep flowing, user messages wait until the supervisor decides Resume/Restart.

 The cooperative scheduler makes all of this lock-free: check-then-park and
 post-then-resume each run without an intervening context switch."
Mailbox := Object [

	| systemQueue userQueue suspended idle running runnerId invoker throughput |


	class new [
		^super new initMailbox
	]


	initMailbox [
		systemQueue := OrderedCollection new.
		userQueue := OrderedCollection new.
		suspended := false.
		idle := false.
		running := true.
		throughput := 30.
		^self
	]


	"configuration"

	invoker: anObject [ invoker := anObject. ^self ]
	throughput: anInteger [ throughput := anInteger. ^self ]


	"posting"

	postUser: aMessage sender: aSenderPid [
		"Enqueue + wake the runner atomically under the global sync monitor (posts come
		 from any worker; the queue is a plain OrderedCollection and there are no concurrent
		 collections yet, so the monitor serializes addLast against the runner's removeFirst)."
		Processor monitorEnter.
		userQueue addLast: (Envelope message: aMessage sender: aSenderPid).
		self schedule.
		Processor monitorExit.
		^self
	]


	postSystem: aMessage [
		Processor monitorEnter.
		systemQueue addLast: aMessage.
		self schedule.
		Processor monitorExit.
		^self
	]


	"suspension (driven by supervision)"

	suspend [ suspended := true. ^self ]

	resume [
		Processor monitorEnter.
		suspended := false.
		self schedule.
		Processor monitorExit.
		^self
	]


	"lifecycle"

	start [
		"Fork the runner fiber that serves this mailbox."
		[ self run ] fork.
		^self
	]


	stopRunner [
		running := false.
		^self
	]


	"scheduling (private)"

	schedule [
		"Wake the parked runner. CALLER HOLDS THE MONITOR. Lost-wakeup-safe: the runner
		 publishes its parked state (parkOnMonitor) before releasing the monitor, so this
		 resumeId, issued under the same monitor, is never lost."
		idle ifTrue: [
			idle := false.
			Processor resumeId: runnerId ].
		^self
	]


	hasProcessable [
		"Read under the monitor (called from run's monitored idle-check)."
		^systemQueue isEmpty not
			or: [ suspended not and: [ userQueue isEmpty not ] ]
	]


	takeSystem [
		"Atomically dequeue the next system message (or nil) under the monitor, so it can't
		 race a concurrent post; the invocation itself runs OUTSIDE the monitor."
		| m |
		Processor monitorEnter.
		m := (running and: [ systemQueue isEmpty not ])
			ifTrue: [ systemQueue removeFirst ] ifFalse: [ nil ].
		Processor monitorExit.
		^m
	]


	takeUser [
		| m |
		Processor monitorEnter.
		m := (running and: [ suspended not and: [ userQueue isEmpty not ] ])
			ifTrue: [ userQueue removeFirst ] ifFalse: [ nil ].
		Processor monitorExit.
		^m
	]


	run [
		Processor monitorEnter.
		runnerId := Processor currentId.
		Processor monitorExit.
		[ running ] whileTrue: [
			self processBatch.
			running ifTrue: [
				Processor monitorEnter.
				self hasProcessable
					ifTrue: [ Processor monitorExit. Processor yield ]
					ifFalse: [ idle := true. Processor parkOnMonitor ] ] ].
		^self
	]


	processBatch [
		"System messages first (always), then a bounded run of user messages. Each message is
		 dequeued under the monitor (take*) but INVOKED outside it — invoke* runs arbitrary
		 actor code that posts/sends, re-entering the (non-recursive) monitor."
		| m count |
		[ (m := self takeSystem) notNil ] whileTrue: [ invoker invokeSystem: m ].
		count := 0.
		[ count < throughput and: [ (m := self takeUser) notNil ] ] whileTrue: [
			invoker invokeUser: m.
			"a user message may enqueue system messages (e.g. a failure); drain them"
			[ (m := self takeSystem) notNil ] whileTrue: [ invoker invokeSystem: m ].
			count := count + 1 ].
		^self
	]
]
