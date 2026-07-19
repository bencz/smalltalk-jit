"QueueStackHeapTest: the wave 2 container facades. Stack is LIFO
 (push:/pop/top over addLast:/removeLast/last), Queue is FIFO
 (enqueue:/dequeue/peek over addLast:/removeFirst/first) and Heap is a binary
 heap whose removeFirst drains in sort-block order (min by default, and by any
 custom block such as a reversed comparator)."

[
	| t st q h r drained ok prev maxh |
	t := TestRun named: 'QueueStackHeap'.

	"---- Stack: LIFO ----"
	st := Stack new.
	st push: 1; push: 2; push: 3.
	t check: 'stack size' equals: 3 is: [ st size ].
	t check: 'stack top is last pushed' equals: 3 is: [ st top ].
	t check: 'stack top does not remove' equals: 3 is: [ st size ].
	t check: 'pop returns last pushed' equals: 3 is: [ st pop ].
	t check: 'pop again' equals: 2 is: [ st pop ].
	t check: 'stack size after two pops' equals: 1 is: [ st size ].
	t check: 'stack top after pops' equals: 1 is: [ st top ].

	"---- Queue: FIFO ----"
	q := Queue new.
	q enqueue: 1; enqueue: 2; enqueue: 3.
	t check: 'queue size' equals: 3 is: [ q size ].
	t check: 'peek is first enqueued' equals: 1 is: [ q peek ].
	t check: 'dequeue returns first enqueued' equals: 1 is: [ q dequeue ].
	t check: 'dequeue again' equals: 2 is: [ q dequeue ].
	t check: 'queue size after two dequeues' equals: 1 is: [ q size ].
	t check: 'peek after dequeues' equals: 3 is: [ q peek ].

	"---- Heap: small deterministic case ----"
	h := Heap new.
	h addAll: #(5 3 8 1 9 2).
	t check: 'heap size' equals: 6 is: [ h size ].
	t check: 'heap first is minimum' equals: 1 is: [ h first ].
	t check: 'heap first does not remove' equals: 6 is: [ h size ].
	t check: 'heap includes an element' that: [ h includes: 8 ].
	t check: 'heap notEmpty' that: [ h notEmpty ].
	t check: 'heap removeFirst is minimum' equals: 1 is: [ h removeFirst ].
	t check: 'heap removeFirst next minimum' equals: 2 is: [ h removeFirst ].

	"---- Heap: random inserts drain in ascending order ----"
	h := Heap new.
	r := Random seed: 20260719.
	1000 timesRepeat: [ h add: (r nextInteger: 100000) ].
	drained := OrderedCollection new.
	[ h isEmpty ] whileFalse: [ drained add: h removeFirst ].
	t check: 'heap drained size matches inserts' equals: 1000 is: [ drained size ].
	t check: 'heap drains in ascending order' that: [ drained isSorted ].

	"---- Heap: reversed sort block yields a max-heap ----"
	maxh := Heap sortBlock: [:x :y | x >= y].
	maxh addAll: #(5 3 8 1 9 2).
	t check: 'max-heap first is maximum' equals: 9 is: [ maxh first ].
	t check: 'max-heap removeFirst is maximum' equals: 9 is: [ maxh removeFirst ].
	drained := OrderedCollection new.
	[ maxh isEmpty ] whileFalse: [ drained add: maxh removeFirst ].
	t check: 'max-heap drains descending' that: [ drained isSorted: [:x :y | x >= y] ].
	t expect: 'empty heap first raises OutOfRangeError' raises: OutOfRangeError in: [ Heap new first ].

	^t report
]
