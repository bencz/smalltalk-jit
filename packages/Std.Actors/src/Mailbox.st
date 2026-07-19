"Each actor has one Mailbox, served by its own fiber (the runner). Messages
 are split into a high-priority system queue and a user queue. The runner drains
 all system messages, then up to `throughput` user messages, then yields for
 fairness; when nothing is processable it parks and is resumed on the next post.

 On failure the mailbox is SUSPENDED: system messages (supervisor directives)
 keep flowing, user messages wait until the supervisor decides Resume/Restart.

 The cooperative scheduler makes all of this lock-free: check-then-park and
 post-then-resume each run without an intervening context switch."
Mailbox := Object [

	| systemQueue userQueue suspended idle running runnerId invoker throughput lastWasSystem |


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
		lastWasSystem := false.
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
		Processor monitorEnterOn: self.
		userQueue addLast: (Envelope message: aMessage sender: aSenderPid).
		self schedule.
		Processor monitorExit.
		^self
	]


	postSystem: aMessage [
		Processor monitorEnterOn: self.
		systemQueue addLast: aMessage.
		self schedule.
		Processor monitorExit.
		^self
	]


	"suspension (driven by supervision)"

	suspend [ suspended := true. ^self ]

	resume [
		Processor monitorEnterOn: self.
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


	takeNext: userCount [
		"Dequeue the next message under ONE monitor hold, SYSTEM PRIORITY: a system
		 message if any (system directives ignore suspension and the throughput bound);
		 otherwise the next user message if the mailbox is running, not suspended, and the
		 per-batch throughput bound is not yet reached. Answer the message or nil, and tag
		 it via lastWasSystem (meaningful only when the answer is notNil). Fully releases
		 the monitor before returning: the invoke that follows runs arbitrary actor code
		 that re-enters the non-recursive monitor. This coalesces the old separate
		 takeSystem + takeUser probes into one acquisition per message."
		| m |
		Processor monitorEnterOn: self.
		(running and: [ systemQueue isEmpty not ])
			ifTrue: [
				lastWasSystem := true.
				m := systemQueue removeFirst ]
			ifFalse: [
				(running and: [ suspended not and: [ userCount < throughput
						and: [ userQueue isEmpty not ] ] ])
					ifTrue: [
						lastWasSystem := false.
						m := userQueue removeFirst ]
					ifFalse: [ m := nil ] ].
		Processor monitorExit.
		^m
	]


	run [
		Processor monitorEnterOn: self.
		runnerId := Processor currentId.
		Processor monitorExit.
		[ running ] whileTrue: [
			self processBatch.
			running ifTrue: [
				Processor monitorEnterOn: self.
				self hasProcessable
					ifTrue: [ Processor monitorExit. Processor yield ]
					ifFalse: [ idle := true. Processor parkOnMonitor ] ] ].
		^self
	]


	processBatch [
		"One loop pulling messages under the monitor (takeNext:) and INVOKING them outside
		 it — invoke* runs arbitrary actor code that posts/sends, re-entering the
		 (non-recursive) monitor. takeNext: gives system messages priority and enforces the
		 throughput bound on user messages, so this reproduces the old 'drain system, then a
		 bounded run of user messages, draining system after each user' with ONE monitor
		 acquisition per message instead of two."
		| item count |
		count := 0.
		[ (item := self takeNext: count) notNil ] whileTrue: [
			lastWasSystem
				ifTrue: [ invoker invokeSystem: item ]
				ifFalse: [ invoker invokeUser: item. count := count + 1 ] ].
		^self
	]
]
