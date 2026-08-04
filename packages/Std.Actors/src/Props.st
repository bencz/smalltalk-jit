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


	"configuration (fluent)

	 EACH ONE ANSWERS A NEW PROPS and leaves the receiver alone, which is what
	 makes the sentence at the top of this file true: a Props is a description
	 that can be handed around and reused. They used to assign in place and
	 answer self, so `base withThroughput: 5` reconfigured the shared `base`
	 for every other spawn site holding it -- the same trap ProtoActor's
	 WithX-returns-a-copy avoids."

	withSupervisor: aStrategy [
		^self copyWithProducer: producer supervisor: aStrategy throughput: throughput
	]


	withThroughput: anInteger [
		^self copyWithProducer: producer supervisor: supervisor throughput: anInteger
	]


	withProducer: aBlock [
		^self copyWithProducer: aBlock supervisor: supervisor throughput: throughput
	]


	copyWithProducer: aBlock supervisor: aStrategy throughput: anInteger [
		| copy |
		copy := self class new setProducer: aBlock.
		copy setSupervisor: aStrategy.
		copy setThroughput: anInteger.
		^copy
	]


	"private, for copyWithProducer:supervisor:throughput: -- these DO assign"

	setSupervisor: aStrategy [ supervisor := aStrategy. ^self ]
	setThroughput: anInteger [ throughput := anInteger. ^self ]
]
