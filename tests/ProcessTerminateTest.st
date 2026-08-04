"ProcessTerminateTest: terminating a process is an UNWIND, not a stack being
 dropped.

 Block>>ensure: promises its block runs 'whether the receiver completes
 normally or is unwound (by an exception, a non-local return, or termination)',
 and Block>>valueUnwindProtected: names Process terminate outright. So every
 pending cleanup between the terminate and the bottom of the fiber has to run,
 innermost first, ON THE STACK THAT REGISTERED IT.

 There are two ways a fiber dies, and they used to be one refusal and one drop:
 terminating YOURSELF, which is a change of stack that never comes back, and
 terminating SOMEBODY ELSE, whose cleanups belong to activations on a stack this
 one is not standing on. Both leave through the same exit record now.

 WHAT IS NOT HERE, and cannot be: the MAIN process dying takes the whole VM
 process with it, so it is checked from outside, by run_tests.sh
 (TERMINATE_STOPS_THE_PROCESS and UNCAUGHT_ERROR_EXITS_NONZERO).

 UnwindTest already pins terminateSelf, terminateParked and unhandledInFiber.
 What is here is what it does not cover: ordering, a victim that never ran, a
 victim that already finished, and the peers going on afterwards."

ProcessTerminateProbe := Object [

	"Run aBlock in a forked process and wait for `aBoolBlock` to answer true.
	 BOUNDED, deliberately: a condition that never comes true has to make the
	 suite RED, not hang it."
	class waitFor: aBoolBlock [
		| tries |
		tries := 0.
		[aBoolBlock value not and: [tries < 20000]] whileTrue: [
			tries := tries + 1.
			Processor yield].
		^aBoolBlock value
	]

]


[
	| t log done process second alive |
	t := TestRun named: 'ProcessTerminate'.

	"NESTED CLEANUPS, INSIDE-OUT, on a fiber terminated from outside. One
	 cleanup would not tell an unwind apart from a single ad-hoc call."
	log := OrderedCollection new.
	done := false.
	process := [[[Processor suspendCurrent]
		ensure: [log add: #inner]]
		ensure: [log add: #outer. done := true]] fork.
	ProcessTerminateProbe waitFor: [process id notNil].
	Processor yield.
	process terminate.
	ProcessTerminateProbe waitFor: [done].
	t check: 'both cleanups ran' equals: 2 is: [log size].
	t check: 'innermost first' equals: #inner is: [log at: 1].
	t check: 'then the outer one' equals: #outer is: [log at: 2].

	"A VICTIM THAT NEVER RAN. newProcess answers a suspended process, and
	 terminating one is not required to wait for it to start. Nothing may run:
	 its chain is empty, so the block itself must not be evaluated."
	log := OrderedCollection new.
	second := [log add: #ranTheBlock] newProcess.
	second terminate.
	Processor yield.
	Processor yield.
	t check: 'a process terminated before it started never runs its block'
		equals: 0 is: [log size].

	"A VICTIM THAT ALREADY FINISHED is not resurrected, and terminating it
	 answers false rather than scheduling a dead fiber."
	log := OrderedCollection new.
	done := false.
	process := [log add: #ran. done := true] fork.
	ProcessTerminateProbe waitFor: [done].
	Processor yield.
	t check: 'terminating a finished process answers false'
		equals: false is: [Processor terminateId: process id].
	t check: 'and does not run it again' equals: 1 is: [log size].

	"AN UNKNOWN ID is an answer, not a failure: the kernel holds a process by
	 id precisely so a stale one can be asked about safely."
	t check: 'terminating an id that names nothing answers false'
		equals: false is: [Processor terminateId: 999999].

	"THE PEERS GO ON. A fiber dying must not take its neighbours with it, which
	 is the property that separates terminate from exit."
	alive := 0.
	done := false.
	process := [[Processor suspendCurrent] ensure: [done := true]] fork.
	[3 timesRepeat: [alive := alive + 1. Processor yield]] fork.
	ProcessTerminateProbe waitFor: [alive = 3].
	process terminate.
	ProcessTerminateProbe waitFor: [done].
	t check: 'a terminated fiber does not stop its peers' equals: 3 is: [alive].
	t check: 'and it did die' equals: true is: [done].

	"SELF-TERMINATE STOPS AT THE TERMINATE. The statement after it must not
	 run: a terminate that answered would be a fiber that kept going."
	log := OrderedCollection new.
	done := false.
	[[Processor thisProcess terminate. log add: #keptGoing]
		ensure: [log add: #cleanup. done := true]] fork.
	ProcessTerminateProbe waitFor: [done].
	t check: 'self-terminate runs the cleanup' equals: true is: [log includes: #cleanup].
	t check: 'and nothing after the terminate' equals: false
		is: [log includes: #keptGoing].

	^t report
]
