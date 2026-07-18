"Tests for Channel: FIFO order, blocking receive, and cross-process handoff,
 driven by the kernel TestRun. The scheduler runs one worker OS thread per
 core, so forked fibers execute in REAL parallelism: cross-fiber results go
 into disjoint slots of a preallocated Array or come back over a dedicated
 reply channel, joins use Semaphores, and blocked states are given real time
 via short Delays, never counted yields."

[ ChannelRun := TestRun named: 'Channel' ]

"buffered sends are received in FIFO order"
[
	| c |
	c := Channel new.
	c send: 1; send: 2; send: 3.
	ChannelRun check: 'three buffered sends are queued' equals: 3 is: [ c size ].
	ChannelRun check: 'first receive answers first send' equals: 1 is: [ c receive ].
	ChannelRun check: 'second receive answers second send' equals: 2 is: [ c receive ].
	ChannelRun check: 'third receive answers third send' equals: 3 is: [ c receive ].
	ChannelRun check: 'channel drains empty' that: [ c isEmpty ].
]

"receive blocks until another process sends; the reply channel carries the
 received value back to the main process"
[
	| c reply |
	c := Channel new.
	reply := Channel new.
	[ reply send: c receive ] fork.
	(Delay forMilliseconds: 20) wait.      "give the consumer time to park in receive"
	ChannelRun check: 'receive stays blocked before any send' that: [ reply isEmpty ].
	c send: #hello.
	ChannelRun check: 'blocked receive wakes with the sent value' equals: #hello is: [ reply receive ].
	ChannelRun check: 'exactly one item came back' that: [ reply isEmpty ].
]

"a producer/consumer handoff over a channel preserves every item and order"
[
	| c received done |
	c := Channel new.
	received := Array new: 5.
	done := Semaphore new.
	[ 1 to: 5 do: [:i | c send: i * 10 ] ] fork.
	[ 1 to: 5 do: [:i | received at: i put: c receive ]. done signal ] fork.
	done wait.
	ChannelRun check: 'every handoff slot was filled' that: [
		| ok | ok := true.
		1 to: 5 do: [:i | (received at: i) isNil ifTrue: [ ok := false ] ].
		ok ].
	ChannelRun check: 'first item arrived in order' equals: 10 is: [ received at: 1 ].
	ChannelRun check: 'last item arrived in order' equals: 50 is: [ received at: 5 ].
]

"multiple consumers each receive distinct items (no double delivery); each
 consumer owns a disjoint slot of the result Array"
[
	| c got done |
	c := Channel new.
	got := Array new: 2.
	done := Semaphore new.
	[ got at: 1 put: c receive. done signal ] fork.
	[ got at: 2 put: c receive. done signal ] fork.
	c send: #x.
	c send: #y.
	done wait. done wait.
	ChannelRun check: 'first consumer got one of the items' that: [
		((got at: 1) = #x) or: [ (got at: 1) = #y ] ].
	ChannelRun check: 'second consumer got one of the items' that: [
		((got at: 2) = #x) or: [ (got at: 2) = #y ] ].
	ChannelRun check: 'no double delivery' that: [ (got at: 1) ~= (got at: 2) ].
]

[ ^ChannelRun report ]
