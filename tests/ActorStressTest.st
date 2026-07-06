"Stress test for the actor system under a heavy message storm. Spawns many
 actors, floods them with fire-and-forget #tell messages, then drains their state
 via request/await round-trips. Every message allocation + mailbox hand-off
 drives the scheduler and the GC; correctness is verified by exact aggregate
 counts on the main process (assertions never live inside an actor, since
 supervision would swallow them)."

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
	Assert true: totalCount = (n * perActor).
	Assert true: totalSum = (n * expectedSum).
	actors do: [:a | a stop ].
]

"request/await fan-out: many concurrent round-trips through echo actors must
 return exactly what was sent, while a churner keeps GC pressure high"
[
	| echoes n garbage ok |
	n := 40.
	echoes := OrderedCollection new.
	1 to: n do: [:i | echoes addLast: (Actors spawn: (Props fromActorClass: StressEcho)) ].

	[ 1 to: 200 do: [:r |
		garbage := OrderedCollection new.
		1 to: 40 do: [:k | garbage addLast: (Array new: 4) ].
		Processor yield ] ] fork.

	ok := 0.
	1 to: n do: [:i |
		| a |
		a := echoes at: i.
		1 to: 25 do: [:k |
			((a request: k * 1000 + i) await) = (k * 1000 + i)
				ifTrue: [ ok := ok + 1 ] ] ].

	Assert true: ok = (n * 25).
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

	Assert true: grandTotal = (waves * perWave * 40).
]
