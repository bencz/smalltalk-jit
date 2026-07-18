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
		"Under the multi-worker pool complete and await race, so both run under the global
		 sync monitor: the completed-check + set + wake is atomic (fixes double-complete and
		 the check-then-park lost-wakeup, exactly like Semaphore signal/wait)."
		Processor monitorEnterOn: self.
		completed ifTrue: [ Processor monitorExit. ^self ].
		completed := true.
		value := aValue.
		waiterId ifNotNil: [:w | Processor resumeId: w ].
		Processor monitorExit.
		^self
	]


	"awaiting"

	await [
		"Park the current process until the reply arrives."
		Processor monitorEnterOn: self.
		completed ifTrue: [ | v | v := value. Processor monitorExit. ^v ].
		waiterId := Processor currentId.
		Processor parkOnMonitor.
		^value
	]


	awaitFor: milliseconds [
		"Park until the reply arrives or the timeout elapses (then answer nil)."
		Processor monitorEnterOn: self.
		completed ifTrue: [ | v | v := value. Processor monitorExit. ^v ].
		waiterId := Processor currentId.
		[ (Delay forMilliseconds: milliseconds) wait. self complete: nil ] fork.
		Processor parkOnMonitor.
		^value
	]
]
