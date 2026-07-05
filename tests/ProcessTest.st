"Tests for green processes (fibers): fork, yield, terminate, identity.
 All assertions run on the main process; forked processes only mutate shared
 state that the main process inspects after yielding (an assertion failing
 inside a forked process would be isolated and would not fail the test)."

"fork answers a Process and actually runs the block"
[
	| ran p |
	ran := 0.
	p := [ ran := ran + 1 ] fork.
	Assert true: (p isKindOf: Process).
	Assert true: ran = 0.        "cooperative: worker has not run yet"
	Processor yield.
	Assert true: ran = 1.        "worker ran while main was yielded"
]

"cooperative interleaving is deterministic and ordered"
[
	| log |
	log := OrderedCollection new.
	[ 1 to: 3 do: [:i | log add: #a. Processor yield] ] fork.
	[ 1 to: 3 do: [:i | log add: #b. Processor yield] ] fork.
	1 to: 8 do: [:i | Processor yield].
	Assert true: log size = 6.
	Assert true: (log at: 1) = #a.
	Assert true: (log at: 2) = #b.
	Assert true: (log at: 3) = #a.
]

"a process id is stable within a process and unique across processes"
[
	| mine other |
	mine := Processor currentId.
	Assert true: mine = Processor currentId.
	other := mine.
	[ other := Processor currentId ] fork.
	Processor yield.
	Assert true: other ~= mine.
]

"terminating a process before it runs prevents it from executing"
[
	| ran p |
	ran := false.
	p := [ ran := true ] fork.
	p terminate.
	Processor yield.
	Processor yield.
	Assert false: ran.
]

"a process can terminate itself; code after terminate does not run"
[
	| reached |
	reached := false.
	[ Processor thisProcess terminate. reached := true ] fork.
	Processor yield.
	Processor yield.
	Assert false: reached.
]
