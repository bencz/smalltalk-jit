"Stress test for the actor system under a heavy message storm, driven by the
 kernel TestRun. Spawns many actors, floods them with fire-and-forget #tell
 messages, then drains their state via request/await round-trips. Every
 message allocation + mailbox hand-off drives the scheduler and the GC;
 correctness is verified by exact aggregate counts on the main process.
 Assertions NEVER live inside an actor: a failing assertion there would be
 caught by supervision and swallowed instead of failing the test. Under the
 multi-worker pool actors run in real parallelism, so all aggregation happens
 on the main process from await results, and helper fibers are joined through
 a Semaphore before the block ends."

[ ActorStressRun := TestRun named: 'ActorStress' ]

"a counter actor that also sums the integer values it is told"
StressCounter := Actor [
	| count sum |
	started: ctx [ count := 0. sum := 0 ]
	receiveMessage: ctx [
		| m |
		m := ctx message.
		(m isKindOf: SmallInteger) ifTrue: [ count := count + 1. sum := sum + m ].
		m = #count ifTrue: [ ctx respond: count ].
		m = #sum ifTrue: [ ctx respond: sum ].
	]
]

"an echo actor for request round-trips"
StressEcho := Actor [
	receiveMessage: ctx [ ctx respond: ctx message ]
]

"message storm: N actors each told M integers; total count and value-sum exact"
[
	| n perActor actors totalCount totalSum expectedSum |
	n := 48.
	perActor := 300.
	actors := OrderedCollection new.
	1 to: n do: [:i | actors addLast: (Actors spawn: (Props fromActorClass: StressCounter)) ].

	1 to: n do: [:i |
		| a |
		a := actors at: i.
		1 to: perActor do: [:k | a tell: k ] ].

	totalCount := 0.
	totalSum := 0.
	1 to: n do: [:i |
		| a |
		a := actors at: i.
		totalCount := totalCount + (a request: #count) await.
		totalSum := totalSum + (a request: #sum) await ].

	expectedSum := perActor * (perActor + 1) // 2.
	ActorStressRun check: 'storm delivered every message' equals: n * perActor is: [ totalCount ].
	ActorStressRun check: 'storm value sum is exact' equals: n * expectedSum is: [ totalSum ].
	actors do: [:a | a stop ].
]

"request/await fan-out: many concurrent round-trips through echo actors must
 return exactly what was sent, while a churner keeps GC pressure high; the
 churner is joined before the block ends so it cannot leak into later blocks"
[
	| echoes n garbage ok churnDone |
	n := 40.
	echoes := OrderedCollection new.
	1 to: n do: [:i | echoes addLast: (Actors spawn: (Props fromActorClass: StressEcho)) ].

	churnDone := Semaphore new.
	[ 1 to: 200 do: [:r |
		garbage := OrderedCollection new.
		1 to: 40 do: [:k | garbage addLast: (Array new: 4) ].
		Processor yield ].
	  churnDone signal ] fork.

	ok := 0.
	1 to: n do: [:i |
		| a |
		a := echoes at: i.
		1 to: 25 do: [:k |
			((a request: k * 1000 + i) await) = (k * 1000 + i)
				ifTrue: [ ok := ok + 1 ] ] ].

	churnDone wait.
	ActorStressRun check: 'echo round-trips all exact' equals: n * 25 is: [ ok ].
	echoes do: [:a | a stop ].
]

"actors spawned, worked, and stopped in waves to churn the registry and GC"
[
	| waves perWave grandTotal |
	waves := 12.
	perWave := 20.
	grandTotal := 0.
	1 to: waves do: [:w |
		| batch |
		batch := OrderedCollection new.
		1 to: perWave do: [:i | batch addLast: (Actors spawn: (Props fromActorClass: StressCounter)) ].
		batch do: [:a | 1 to: 40 do: [:k | a tell: k ] ].
		batch do: [:a | grandTotal := grandTotal + (a request: #count) await ].
		batch do: [:a | a stop ] ].

	ActorStressRun check: 'wave churn counted every message' equals: waves * perWave * 40 is: [ grandTotal ].
]

[ ^ActorStressRun report ]
