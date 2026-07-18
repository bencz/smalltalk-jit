"An unbounded blocking channel (a la Go channels / actor mailboxes). Senders
 never block; a receiver blocks until an item is available. Safe for multiple
 producers and consumers across the multi-worker scheduler pool: the backing queue
 is mutated only under the global sync monitor, and item availability is tracked by
 an internal Semaphore (itself monitor-protected). The queue mutation and the
 semaphore op are kept as separate flat critical sections (never nested), since the
 monitor is not re-entrant.
 WARNING: with multiple receivers an item goes to WHICHEVER receiver consumes a
 permit first; there is no addressing. In particular, never use one channel for
 both request and reply traffic: the requester's receive races the consumer for
 the requester's own still-queued items and can steal one as the 'reply'
 (deterministic only under a single cooperative worker, which is not the default).
 Use a dedicated reply channel per direction."
Channel := Object [

	| queue itemsAvailable |


	class new [
		^super new initChannel
	]


	initChannel [
		queue := OrderedCollection new.
		itemsAvailable := Semaphore new.
		^self
	]


	"sending"

	send: anObject [
		Processor monitorEnterOn: self.
		queue addLast: anObject.
		Processor monitorExit.
		itemsAvailable signal.
		^anObject
	]


	"receiving"

	receive [
		"Block the current process until an item is available, then return it. The wait
		 returns only after a matching send has enqueued its item (send does addLast
		 before signal), so removeFirst under the monitor always finds an element."
		| item |
		itemsAvailable wait.
		Processor monitorEnterOn: self.
		item := queue removeFirst.
		Processor monitorExit.
		^item
	]


	"testing (advisory snapshots)"

	isEmpty [
		| empty |
		Processor monitorEnterOn: self.
		empty := queue isEmpty.
		Processor monitorExit.
		^empty
	]


	size [
		| n |
		Processor monitorEnterOn: self.
		n := queue size.
		Processor monitorExit.
		^n
	]

]
