"Supervision strategies. A strategy inspects a child's failure and decides a
 directive by running its `decider` block on the failure reason:
   #resume   - ignore the error, keep going with the same actor state
   #restart  - recreate the actor (state reset) and continue
   #stop     - stop the failed child
   #escalate - treat it as the supervisor's own failure

 The default decider is nil, meaning 'always #restart'. IMPORTANT: the kernel
 never stores a block here at bootstrap (a block serialised into the snapshot
 would reference dead bootstrap contexts); user-supplied decider blocks are
 created at runtime and are fine.

 A RESTART IS BOUNDED, which is the difference between supervision and a spin
 loop. An actor that fails every time it starts was restarted forever: measured
 at 100000 restarts in 500 ms, one worker pinned and nothing else making
 progress, and no report anywhere that anything was wrong. ProtoActor answers
 this with maxNrOfRetries within a time span on the strategy plus a
 RestartStatistics per child, and so does this: past the budget the directive
 becomes #stop.

 v1 ships OneForOneStrategy (acts on the failed child only)."

SupervisorStrategy := Object [

	handleFailure: aFailure context: aContext [
		self subclassResponsibility
	]
]


RestartStatistics := Object [
	"One per actor, kept by its ActorContext and carried on the ActorFailure so
	 the supervisor (which is shared, and may govern many children) does not
	 have to keep a table keyed by child. Holds the times of recent failures, so
	 the question a strategy asks -- how many times has THIS child failed in the
	 last N milliseconds -- is answered without a periodic sweep."

	| failureTimes |


	class new [
		^super new initStatistics
	]


	class nowMilliseconds [
		"Monotonic, so a wall-clock adjustment cannot make a restart budget
		 look spent or unspent."
		^System monotonicNanos // 1000000
	]


	initStatistics [
		failureTimes := OrderedCollection new.
		^self
	]


	fail [
		failureTimes addLast: RestartStatistics nowMilliseconds.
		^self
	]


	reset [
		failureTimes := OrderedCollection new.
		^self
	]


	failureCount [ ^failureTimes size ]


	numberOfFailuresWithin: milliseconds [
		"nil means no window: every failure ever recorded counts."
		| cutoff n |
		milliseconds isNil ifTrue: [ ^failureTimes size ].
		cutoff := RestartStatistics nowMilliseconds - milliseconds.
		n := 0.
		failureTimes do: [:t | t >= cutoff ifTrue: [ n := n + 1 ] ].
		^n
	]
]


OneForOneStrategy := SupervisorStrategy [

	| decider maxRetries withinMilliseconds |


	class new [
		^super new initStrategy
	]


	class default [
		"Restart on any error, up to the default budget. decider stays nil so
		 nothing is captured."
		^self new
	]


	class decider: aBlock [
		"aBlock maps a failure reason to a directive symbol. Create it at
		 runtime, never during bootstrap."
		^self new setDecider: aBlock
	]


	initStrategy [
		"ProtoActor's default budget: ten restarts within ten seconds. Numbers,
		 not blocks, so this is safe to build at bootstrap."
		maxRetries := 10.
		withinMilliseconds := 10000.
		^self
	]


	setDecider: aBlock [
		decider := aBlock.
		^self
	]


	"accessing the budget"

	maxRetries [ ^maxRetries ]
	withinMilliseconds [ ^withinMilliseconds ]


	withMaxRetries: anIntegerOrNil within: millisecondsOrNil [
		"nil retries means unbounded (the old behaviour, and a spin loop when
		 the actor fails on start); nil milliseconds means no time window, so
		 every failure the child ever had counts against the budget."
		maxRetries := anIntegerOrNil.
		withinMilliseconds := millisecondsOrNil.
		^self
	]


	directiveFor: aReason [
		decider isNil ifTrue: [ ^#restart ].
		^decider value: aReason
	]


	exceededBudget: aStatistics [
		"Charge this failure to the child and answer whether it has now spent
		 its budget. Only asked when the directive is #restart: #resume, #stop
		 and #escalate do not consume restarts."
		aStatistics isNil ifTrue: [ ^false ].
		maxRetries isNil ifTrue: [ ^false ].
		aStatistics fail.
		(aStatistics numberOfFailuresWithin: withinMilliseconds) > maxRetries
			ifTrue: [
				"Reset, so a child that IS stopped here does not carry a spent
				 budget into whatever supervises it next."
				aStatistics reset.
				^true ].
		^false
	]


	handleFailure: aFailure context: aContext [
		| directive child |
		child := aFailure child.
		directive := self directiveFor: aFailure reason.

		"A restart budget is what keeps an actor that fails on every start from
		 restarting in a tight loop forever."
		directive == #restart ifTrue: [
			(self exceededBudget: aFailure statistics)
				ifTrue: [ directive := #stop ] ].

		directive == #resume   ifTrue: [ ^child sendSystem: SystemResume ].
		directive == #restart  ifTrue: [ ^child sendSystem: SystemRestart ].
		directive == #stop     ifTrue: [ ^child sendSystem: SystemStop ].
		directive == #escalate ifTrue: [ ^aContext escalate: aFailure ].

		"unknown directive: safest default is to restart"
		^child sendSystem: SystemRestart
	]
]
