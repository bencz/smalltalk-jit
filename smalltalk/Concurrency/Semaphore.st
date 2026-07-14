"A counting semaphore. Under the multi-worker scheduler pool wait/signal are NOT
 automatically atomic (fibers run on several OS threads), so 'check the count then
 enqueue-and-park' and 'wake a waiter or bump the count' each run under the global
 sync monitor. Parking uses parkOnMonitor, which drops the monitor only after the
 parked state is published, so a concurrent signal can never lose the wakeup."
Semaphore := Object [

	| count waiting |


	"instance creation"

	class new [
		^super new initSemaphore
	]


	class forMutualExclusion [
		"A semaphore with a single excess signal, i.e. a mutex."
		| semaphore |
		semaphore := self new.
		semaphore signal.
		^semaphore
	]


	"initialization"

	initSemaphore [
		count := 0.
		waiting := OrderedCollection new.
		^self
	]


	"synchronization"

	wait [
		"Decrement the semaphore, blocking the current process while it is zero."
		Processor monitorEnter.
		count > 0
			ifTrue: [
				count := count - 1.
				Processor monitorExit ]
			ifFalse: [
				"Enqueue self and park; parkOnMonitor releases the monitor atomically
				 with publishing the parked state (no re-acquire on wake). A signaller
				 must hold the monitor to dequeue+resume us, so it hands off directly
				 without touching count — hence no monitorExit / decrement here."
				waiting addLast: Processor currentId.
				Processor parkOnMonitor ].
		^self
	]


	signal [
		"Wake one waiting process, or bump the count if none are waiting."
		Processor monitorEnter.
		waiting isEmpty
			ifTrue: [ count := count + 1 ]
			ifFalse: [ Processor resumeId: waiting removeFirst ].
		Processor monitorExit.
		^self
	]


	critical: aBlock [
		"Evaluate aBlock in mutual exclusion; releases even if aBlock unwinds."
		self wait.
		^aBlock ensure: [ self signal ]
	]

]
