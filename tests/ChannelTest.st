"Tests for Channel: FIFO order, blocking receive, and cross-process handoff."

"buffered sends are received in FIFO order"
[
	| c |
	c := Channel new.
	c send: 1; send: 2; send: 3.
	Assert true: c size = 3.
	Assert true: c receive = 1.
	Assert true: c receive = 2.
	Assert true: c receive = 3.
	Assert true: c isEmpty.
]

"receive blocks until another process sends"
[
	| c log |
	c := Channel new.
	log := OrderedCollection new.
	[ log add: (c receive) ] fork.
	Processor yield.              "worker blocks in receive"
	Assert true: log isEmpty.
	c send: #hello.
	Processor yield.              "worker wakes with the value"
	Assert true: log size = 1.
	Assert true: (log at: 1) = #hello.
]

"a producer/consumer handoff over a channel preserves every item and order"
[
	| c received |
	c := Channel new.
	received := OrderedCollection new.
	[ 1 to: 5 do: [:i | c send: i * 10] ] fork.
	[ 1 to: 5 do: [:i | received add: c receive] ] fork.
	1 to: 20 do: [:i | Processor yield].
	Assert true: received size = 5.
	Assert true: (received at: 1) = 10.
	Assert true: (received at: 5) = 50.
]

"multiple consumers each receive distinct items (no double delivery)"
[
	| c a b |
	c := Channel new.
	a := OrderedCollection new.
	b := OrderedCollection new.
	[ a add: c receive ] fork.
	[ b add: c receive ] fork.
	Processor yield.             "both consumers block"
	c send: #x.
	c send: #y.
	1 to: 4 do: [:i | Processor yield].
	Assert true: a size = 1.
	Assert true: b size = 1.
	Assert true: (a at: 1) ~= (b at: 1).
]
