"============================================================
  15 - Stack and Queue
  ------------------------------------------------------------
  Two fundamental abstract data types implemented on top of an
  OrderedCollection: a LIFO Stack and a FIFO Queue.
  ============================================================"

Stack := Object [
	| items |

	class new [ ^super new init ]
	init [ items := OrderedCollection new ]

	push: anObject [ items addLast: anObject. ^anObject ]
	pop [ self isEmpty ifTrue: [^self error: 'pop from empty stack'].
	      ^items removeLast ]
	top [ ^items last ]
	isEmpty [ ^items isEmpty ]
	size [ ^items size ]
]

Queue := Object [
	| items |

	class new [ ^super new init ]
	init [ items := OrderedCollection new ]

	enqueue: anObject [ items addLast: anObject. ^anObject ]
	dequeue [ self isEmpty ifTrue: [^self error: 'dequeue from empty queue'].
	          ^items removeFirst ]
	front [ ^items first ]
	isEmpty [ ^items isEmpty ]
	size [ ^items size ]
]

"---- Stack: last in, first out ----"
[
	| s |
	s := Stack new.
	s push: 1; push: 2; push: 3.
	Transcript nextPutAll: 'stack size = '. s size printNl.
	Transcript nextPutAll: 'pop -> '. s pop printNl.
	Transcript nextPutAll: 'pop -> '. s pop printNl.
	Transcript nextPutAll: 'top -> '. s top printNl.
]

"---- Queue: first in, first out ----"
[
	| q |
	q := Queue new.
	q enqueue: 'a'; enqueue: 'b'; enqueue: 'c'.
	Transcript nextPutAll: 'dequeue -> '; nextPutAll: q dequeue; lf.
	Transcript nextPutAll: 'dequeue -> '; nextPutAll: q dequeue; lf.
	Transcript nextPutAll: 'front   -> '; nextPutAll: q front; lf.
]

"---- Using the Stack to reverse a sequence ----"
[
	| s reversed |
	s := Stack new.
	#(1 2 3 4 5) do: [:each | s push: each].
	reversed := OrderedCollection new.
	[s isEmpty] whileFalse: [reversed add: s pop].
	Transcript nextPutAll: 'reversed via stack = '. reversed printNl.
]
