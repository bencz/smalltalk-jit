"Multi-worker stress for the generic Atomics facility: proves lock-free correctness
 under REAL OS threads (Worker parallel:, shared heap).

  1. fetch-add:  N workers x M incrementAndGet on ONE shared AtomicInteger -> the
     final value must be exactly N*M (any lost update = a torn read-modify-write).
  2. CAS loop:   N workers x M compareAndSet: retry-increments on a shared counter
     -> also exactly N*M (proves compareAndSet: under contention).
  3. Treiber stack: a real lock-free stack built ONLY on Atomic>>compareAndSet:with:.
     N workers concurrently push M nodes each (disjoint value ranges, with allocation
     churn so scavenges promote the young nodes and fire the generational write
     barrier on the shared cell), then N workers concurrently pop until empty. Every
     node must be popped exactly once: total count = N*M, total sum = sum(1..N*M),
     stack empty. A missed barrier (dangling young node reclaimed) or a torn CAS
     shows up as a wrong sum/count.

 Worker blocks only return numbers (no asserts cross the thread boundary); the main
 fiber checks and returns the failure count.

 WARNING: drives real OS threads; run ONLY inside the resource sandbox (systemd-run
 + taskset), never loose (project rule after the 2026-07-13 desktop freeze)."

AtomNode := Object [
	| value next |
	class value: v [ ^self new setValue: v ]
	setValue: v [ value := v. ^self ]
	value [ ^value ]
	next [ ^next ]
	next: n [ next := n. ^self ]
]

AtomStack := Object [
	| cell |
	class new [ ^super new initStack ]
	initStack [ cell := Atomic new. ^self ]

	push: v [
		| node done |
		node := AtomNode value: v.
		done := false.
		[ done not ] whileTrue: [
			| head |
			head := cell get.
			node next: head.
			done := cell compareAndSet: head with: node ].
		^v
	]

	pop [
		"Answer the top value, or nil if empty. No node is ever reused (each push
		 allocates fresh), so a popped node can never reappear -> no ABA."
		| done result |
		done := false.
		result := nil.
		[ done not ] whileTrue: [
			| head |
			head := cell get.
			head isNil
				ifTrue: [ done := true ]
				ifFalse: [ (cell compareAndSet: head with: head next)
					ifTrue: [ done := true. result := head value ] ] ].
		^result
	]

	isEmpty [ ^cell get isNil ]
]

[ | fails check nWorkers perWorker counter results total perStack stack expectedSum expectedCount gotSum gotCount aa slotSum |
	fails := 0.
	check := [:cond :label |
		cond ifFalse: [ fails := fails + 1. Transcript nextPutAll: 'FAIL: '; nextPutAll: label; lf ] ].
	nWorkers := 4.

	"---- 1. fetch-add under contention ---------------------------------------"
	perWorker := 25000.
	counter := AtomicInteger new.
	results := Worker parallel: ((1 to: nWorkers) collect: [:w |
		[ 1 to: perWorker do: [:i | counter incrementAndGet ]. perWorker ] ]).
	total := nWorkers * perWorker.
	check value: (counter get = total) value: 'fetch-add: no lost increments'.

	"---- 2. compareAndSet: retry loop ----------------------------------------"
	counter := AtomicInteger new.
	Worker parallel: ((1 to: nWorkers) collect: [:w |
		[ 1 to: perWorker do: [:i |
			| done | done := false.
			[ done not ] whileTrue: [
				| h | h := counter get. done := counter compareAndSet: h with: h + 1 ] ].
		  perWorker ] ]).
	check value: (counter get = total) value: 'CAS loop: no lost increments'.

	"---- 3. Treiber stack: concurrent push, then concurrent pop --------------"
	perStack := 5000.
	stack := AtomStack new.
	expectedCount := nWorkers * perStack.
	expectedSum := expectedCount * (expectedCount + 1) / 2.

	"push: worker w owns values ((w-1)*perStack + 1) .. (w*perStack); together they
	 are exactly 1 .. expectedCount, each pushed once."
	Worker parallel: ((1 to: nWorkers) collect: [:w |
		[ | base |
		  base := (w - 1) * perStack.
		  1 to: perStack do: [:i |
			stack push: base + i.
			i \\ 64 = 0 ifTrue: [ Array new: 16 ] ].   "churn -> scavenge -> promote nodes"
		  perStack ] ]).

	"pop: each worker drains until nil, returning (count -> sum)."
	results := Worker parallel: ((1 to: nWorkers) collect: [:w |
		[ | sum count done |
		  sum := 0. count := 0. done := false.
		  [ done not ] whileTrue: [
			| v | v := stack pop.
			v isNil ifTrue: [ done := true ]
				ifFalse: [ sum := sum + v. count := count + 1 ] ].
		  count -> sum ] ]).

	gotCount := 0. gotSum := 0.
	results do: [:r | gotCount := gotCount + r key. gotSum := gotSum + r value ].
	check value: (gotCount = expectedCount) value: 'Treiber: every node popped exactly once (count)'.
	check value: (gotSum = expectedSum) value: 'Treiber: no lost/duplicated node (sum)'.
	check value: (stack isEmpty) value: 'Treiber: stack drained empty'.

	"---- 4. AtomicArray: contended shared slot + per-worker slots -------------"
	aa := AtomicArray new: nWorkers.
	1 to: nWorkers do: [:w | aa at: w put: 0 ].
	Worker parallel: ((1 to: nWorkers) collect: [:w |
		[ 1 to: perWorker do: [:i |
			aa at: 1 getAndAdd: 1.      "contended: every worker hammers slot 1"
			aa at: w getAndAdd: 1 ].    "per-worker slot (w=1 also lands on slot 1)"
		  perWorker ] ]).
	"slot 1 = N*M (shared) + M (worker 1's own slot IS slot 1) = (N+1)*M"
	check value: ((aa at: 1) = ((nWorkers + 1) * perWorker))
		value: 'AtomicArray: contended slot has no lost add'.
	slotSum := 0.
	2 to: nWorkers do: [:w | slotSum := slotSum + (aa at: w) ].
	check value: (slotSum = ((nWorkers - 1) * perWorker))
		value: 'AtomicArray: per-slot fetch-add exact'.

	fails = 0 ifTrue: [ Transcript nextPutAll: 'AtomicStressTest: OK'; lf ].
	^fails
]
