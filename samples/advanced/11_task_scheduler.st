"============================================================
  ADVANCED - Cooperative Task Scheduler
  ------------------------------------------------------------
  A scheduler that knows nothing about the work it runs.

  It holds a queue of tasks and repeatedly asks the most urgent
  unfinished one to take a single `step'. Every task answers
  `step' differently, so the scheduler needs no conditionals and
  no knowledge of task types: adding a new kind of work means
  adding a subclass, not editing the scheduler. That is the
  whole point of polymorphic dispatch.

  This is the pattern the Richards benchmark exercises, which is
  why it is such a good measure of a VM's message send: the inner
  loop is nothing but dynamic dispatch. See benchmarks/README.md.
  ============================================================"

"---- The scheduler: entirely generic ----"
Scheduler := Object [
	| tasks ticks |

	class new [ ^super new setUp ]
	setUp [ tasks := OrderedCollection new. ticks := 0 ]

	add: aTask [ tasks add: aTask. ^aTask ]
	ticks [ ^ticks ]

	"The most urgent task that still has work left, or nil if all are done."
	nextTask [
		| best |
		best := nil.
		tasks do: [:each |
			each isDone ifFalse: [
				(best isNil or: [each priority > best priority])
					ifTrue: [best := each]]].
		^best
	]

	"The whole engine: ask, do not tell. `step' is the only thing it knows."
	run [
		| task |
		[task := self nextTask. task isNil] whileFalse: [
			ticks := ticks + 1.
			task step]
	]
]

"---- A task: only `step' and `isDone' are required ----"
Task := Object [
	| name priority |

	name [ ^name ]
	name: aString [ name := aString ]
	priority [ ^priority ]
	priority: anInteger [ priority := anInteger ]

	step [ ^self subclassResponsibility ]
	isDone [ ^self subclassResponsibility ]

	log: aString [
		Transcript nextPutAll: '  '; nextPutAll: name; nextPutAll: ': ';
			nextPutAll: aString; lf
	]
]

"---- Counts down, one number per step ----"
Countdown := Task [
	| remaining |

	class named: aString from: n priority: p [
		^self new name: aString; priority: p; setFrom: n; yourself
	]
	setFrom: n [ remaining := n ]

	isDone [ ^remaining <= 0 ]
	step [
		self log: remaining printString.
		remaining := remaining - 1
	]
]

"---- Drains a collection, one element per step ----"
Drain := Task [
	| items seen |

	class named: aString over: aCollection priority: p [
		^self new name: aString; priority: p; setItems: aCollection; yourself
	]
	setItems: aCollection [
		items := aCollection asOrderedCollection.
		seen := OrderedCollection new
	]

	seen [ ^seen ]
	isDone [ ^items isEmpty ]
	step [
		| item |
		item := items removeFirst.
		seen add: item.
		self log: 'took ' , item printString
	]
]

"---- Runs a block until the block says it is finished ----"
Poll := Task [
	| block done |

	class named: aString until: aBlock priority: p [
		^self new name: aString; priority: p; setBlock: aBlock; yourself
	]
	setBlock: aBlock [ block := aBlock. done := false ]

	isDone [ ^done ]
	step [
		done := block value.
		self log: (done ifTrue: ['ready'] ifFalse: ['waiting...'])
	]
]

[
	| sched drain pollCount |

	Transcript nextPutAll: 'Three unrelated task types, one dumb scheduler:'; lf.

	sched := Scheduler new.
	pollCount := 0.

	"Higher priority runs first, and runs to completion before the next starts,
	 because the scheduler always re-picks the most urgent unfinished task."
	sched add: (Countdown named: 'countdown' from: 3 priority: 30).
	drain := sched add: (Drain named: 'drain' over: #('a' 'b' 'c') priority: 20).
	sched add: (Poll named: 'poll'
		until: [pollCount := pollCount + 1. pollCount >= 2]
		priority: 10).

	sched run.

	Transcript lf; nextPutAll: 'every task reported done after ';
		nextPutAll: sched ticks printString; nextPutAll: ' steps'; lf.
	Transcript nextPutAll: 'drain saw: '. drain seen printNl.

	"Adding a fourth kind of task would not change one line of Scheduler."
]
