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

 v1 ships OneForOneStrategy (acts on the failed child only)."

SupervisorStrategy := Object [

	handleFailure: aFailure context: aContext [
		self subclassResponsibility
	]
]


OneForOneStrategy := SupervisorStrategy [

	| decider |


	class default [
		"Restart on any error. decider stays nil so nothing is captured."
		^self new
	]


	class decider: aBlock [
		"aBlock maps a failure reason to a directive symbol. Create it at
		 runtime, never during bootstrap."
		^self new setDecider: aBlock
	]


	setDecider: aBlock [
		decider := aBlock.
		^self
	]


	directiveFor: aReason [
		decider isNil ifTrue: [ ^#restart ].
		^decider value: aReason
	]


	handleFailure: aFailure context: aContext [
		| directive child |
		child := aFailure child.
		directive := self directiveFor: aFailure reason.

		directive == #resume   ifTrue: [ ^child sendSystem: SystemResume ].
		directive == #restart  ifTrue: [ ^child sendSystem: SystemRestart ].
		directive == #stop     ifTrue: [ ^child sendSystem: SystemStop ].
		directive == #escalate ifTrue: [ ^aContext escalate: aFailure ].

		"unknown directive: safest default is to restart"
		^child sendSystem: SystemRestart
	]
]
