"Props is the recipe for spawning an actor: how to build the actor instance,
 which supervisor strategy governs its children, and its mailbox throughput.
 Props are immutable descriptions and can be reused to spawn many actors."
Props := Object [

	| producer supervisor throughput |


	class fromProducer: aBlock [
		"aBlock answers a fresh actor instance each time it is evaluated."
		^self new setProducer: aBlock
	]


	class fromActorClass: aClass [
		^self new setProducer: [ aClass new ]
	]


	setProducer: aBlock [
		producer := aBlock.
		supervisor := OneForOneStrategy default.
		throughput := 30.
		^self
	]


	"accessing"

	producer [ ^producer ]
	supervisor [ ^supervisor ]
	throughput [ ^throughput ]


	"configuration (fluent)"

	withSupervisor: aStrategy [ supervisor := aStrategy. ^self ]
	withThroughput: anInteger [ throughput := anInteger. ^self ]
]
