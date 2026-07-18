"Single-threaded semantics of the generic Atomics facility (Atomic / AtomicInteger).
 Counts failures and returns the count so the runner catches a failure (an Assert
 that raises would terminate the fiber with exit 0 and be reported as a pass). The
 lock-free / GC-barrier behaviour under real workers lives in AtomicStressTest.st,
 which the runner drives inside the resource sandbox."

[ | fails check a i j n |
	fails := 0.
	check := [:cond :label |
		cond ifFalse: [ fails := fails + 1. Transcript nextPutAll: 'FAIL: '; nextPutAll: label; lf ] ].

	"---- Atomic: generic cell -------------------------------------------------"
	a := Atomic new.
	check value: (a get isNil) value: 'new get is nil'.

	a := Atomic on: 42.
	check value: (a get = 42) value: 'on: 42'.

	a set: 7.
	check value: (a get = 7) value: 'set: 7'.

	check value: ((a getAndSet: 9) = 7) value: 'getAndSet: returns old'.
	check value: (a get = 9) value: 'getAndSet: stored new'.

	check value: ((a compareAndSet: 9 with: 10) = true) value: 'CAS hit returns true'.
	check value: (a get = 10) value: 'CAS hit stored new'.

	check value: ((a compareAndSet: 999 with: 11) = false) value: 'CAS miss returns false'.
	check value: (a get = 10) value: 'CAS miss left value'.

	"---- Atomic: object references (identity) ---------------------------------"
	i := Object new.
	j := Object new.
	a set: i.
	check value: (a get == i) value: 'holds object ref (identity)'.
	check value: ((a compareAndSet: i with: j) = true) value: 'CAS ref hit'.
	check value: (a get == j) value: 'CAS ref stored new'.
	check value: ((a compareAndSet: i with: i) = false) value: 'CAS ref miss'.
	check value: (a get == j) value: 'CAS ref miss left value'.

	"---- AtomicInteger: fetch-add --------------------------------------------"
	n := AtomicInteger new.
	check value: (n get = 0) value: 'AtomicInteger new is 0'.

	n := AtomicInteger on: 100.
	check value: (n get = 100) value: 'AtomicInteger on: 100'.

	check value: ((n getAndAdd: 5) = 100) value: 'getAndAdd: returns old'.
	check value: (n get = 105) value: 'getAndAdd: applied'.

	check value: ((n addAndGet: 5) = 110) value: 'addAndGet: returns new'.
	check value: (n get = 110) value: 'addAndGet: applied'.

	check value: ((n incrementAndGet) = 111) value: 'incrementAndGet'.
	check value: ((n getAndIncrement) = 111) value: 'getAndIncrement returns old'.
	check value: (n get = 112) value: 'getAndIncrement applied'.

	check value: ((n decrementAndGet) = 111) value: 'decrementAndGet'.
	check value: ((n getAndDecrement) = 111) value: 'getAndDecrement returns old'.
	check value: (n get = 110) value: 'getAndDecrement applied'.

	"negative delta"
	check value: ((n getAndAdd: -20) = 110) value: 'getAndAdd: negative returns old'.
	check value: (n get = 90) value: 'getAndAdd: negative applied'.

	"inherited CAS on an AtomicInteger"
	check value: ((n compareAndSet: 90 with: 200) = true) value: 'AtomicInteger CAS hit'.
	check value: (n get = 200) value: 'AtomicInteger CAS applied'.

	"a run of increments lands exactly"
	n := AtomicInteger new.
	1 to: 1000 do: [:k | n incrementAndGet ].
	check value: (n get = 1000) value: 'serial 1000 increments'.

	fails = 0 ifTrue: [ Transcript nextPutAll: 'AtomicTest: OK'; lf ].
	^fails
]
