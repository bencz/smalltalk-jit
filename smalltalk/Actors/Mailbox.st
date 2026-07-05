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
		userQueue addLast: (Envelope message: aMessage sender: aSenderPid).
		self schedule.
		^self
	]


	postSystem: aMessage [
		systemQueue addLast: aMessage.
		self schedule.
		^self
	]


	"suspension (driven by supervision)"

	suspend [ suspended := true. ^self ]

	resume [
		suspended := false.
		self schedule.
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
		"Wake the runner if it is parked. Cooperative scheduling makes the
		 idle-check and resume atomic (no yield in between)."
		idle ifTrue: [
			idle := false.
			Processor resumeId: runnerId ].
		^self
	]


	hasProcessable [
		^systemQueue isEmpty not
			or: [ suspended not and: [ userQueue isEmpty not ] ]
	]


	run [
		runnerId := Processor currentId.
		[ running ] whileTrue: [
			self processBatch.
			running ifTrue: [
				self hasProcessable
					ifTrue: [ Processor yield ]
					ifFalse: [
						idle := true.
						Processor suspendCurrent.
						idle := false ] ] ].
		^self
	]


	processBatch [
		"System messages first (always), then a bounded run of user messages."
		[ running and: [ systemQueue isEmpty not ] ] whileTrue: [
			invoker invokeSystem: systemQueue removeFirst ].

		(running and: [ suspended not ]) ifTrue: [
			| count |
			count := 0.
			[ running and: [ suspended not
				and: [ count < throughput and: [ userQueue isEmpty not ] ] ] ] whileTrue: [
					invoker invokeUser: userQueue removeFirst.
					"a user message may enqueue system messages (e.g. a failure); drain them"
					[ running and: [ systemQueue isEmpty not ] ] whileTrue: [
						invoker invokeSystem: systemQueue removeFirst ].
					count := count + 1 ] ].
		^self
	]
]
