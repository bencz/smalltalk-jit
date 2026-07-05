"A counting semaphore. Because the scheduler is cooperative and single
 threaded, wait/signal need no locks: the sequence 'enqueue self then suspend'
 runs atomically with respect to other processes."
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
		count > 0
			ifTrue: [ count := count - 1 ]
			ifFalse: [
				waiting addLast: Processor currentId.
				Processor suspendCurrent ].
		^self
	]


	signal [
		"Wake one waiting process, or bump the count if none are waiting."
		waiting isEmpty
			ifTrue: [ count := count + 1 ]
			ifFalse: [ Processor resumeId: waiting removeFirst ].
		^self
	]


	critical: aBlock [
		"Evaluate aBlock in mutual exclusion; releases even if aBlock unwinds."
		self wait.
		^aBlock ensure: [ self signal ]
	]

]
