Iterator := Object [

	"enumerating"

	do: aBlock [
		SubClassResponsibility signal.
	]


	select: aBlock [
		SubClassResponsibility signal.
	]


	collect: aBlock [
		SubClassResponsibility signal.
	]


	inject: initialValue into: aBlock [
		| result |

		result := initialValue.
		self do: [ :v | result := aBlock value: result value: v].
		^result
	]


	detect: aBlock [
		^self detect: aBlock ifNone: [NotFoundError signal]
	]


	detect: aBlock ifNone: aNoneBlock [
		self do: [ :v | (aBlock value: v) ifTrue: [^v]].
		^aNoneBlock value
	]


	anySatisfy: aBlock [
		self do: [ :v | (aBlock value: v) ifTrue: [^true]].
		^false
	]


	"lazy pipelines"

	asLazy [
		"Start a LINQ-style deferred pipeline over the receiver. Because every
		 Collection and every Stream is an Iterator, they all get this for free."
		^LazySequence on: self
	]


	lazy [
		"Alias of #asLazy."
		^self asLazy
	]

]
