"A Future is the reply side of request/response. It is a single-use endpoint: it
 carries its own *direct* Pid (so replies reach it without touching the system
 registry — no register/unregister, no dead-letter routing) and it IS its own
 synchronization (the value is stored here and the one waiter is parked/resumed
 directly), so a round-trip allocates just the Future and its Pid. Correct under
 the cooperative scheduler: check-then-park and complete-then-resume each run
 without an intervening context switch, exactly like Semaphore."
Future := Object [

	| pid completed value waiterId |


	class in: aSystem [
		^self new initIn: aSystem
	]


	initIn: aSystem [
		completed := false.
		"a direct pid: delivery uses `pid endpoint` (this Future) and never the
		 registry. Keep a real id so Pid>>= / hash stay valid."
		pid := Pid system: aSystem id: aSystem nextId endpoint: self.
		^self
	]


	pid [ ^pid ]


	"endpoint protocol (the system delivers replies through this)"

	postUser: aMessage sender: aSenderPid [
		self complete: aMessage.
		^self
	]

	postSystem: aMessage [ ^self ]


	"completion"

	complete: aValue [
		completed ifTrue: [ ^self ].
		completed := true.
		value := aValue.
		waiterId ifNotNil: [:w | Processor resumeId: w ].
		^self
	]


	"awaiting"

	await [
		"Park the current process until the reply arrives."
		completed ifTrue: [ ^value ].
		waiterId := Processor currentId.
		Processor suspendCurrent.
		^value
	]


	awaitFor: milliseconds [
		"Park until the reply arrives or the timeout elapses (then answer nil)."
		completed ifTrue: [ ^value ].
		waiterId := Processor currentId.
		[ (Delay forMilliseconds: milliseconds) wait. self complete: nil ] fork.
		Processor suspendCurrent.
		^value
	]
]
