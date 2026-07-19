"A tiny publish/subscribe bus: handlers are plain blocks, payloads are
 whatever object the publisher hands over (Domain orders, Storage errors:
 namespaces scope NAMES, instances flow freely)."
Events := Namespace [

	EventBus := Object [

		| subscribers |

		class new [
			^self basicNew initBus
		]

		initBus [
			subscribers := Dictionary new.
			^self
		]

		on: aTopic do: aBlock [
			(subscribers at: aTopic ifAbsentPut: [OrderedCollection new]) add: aBlock.
		]

		publish: aTopic with: aPayload [
			"deliver to every handler; answer how many saw it"
			| handlers |
			handlers := subscribers at: aTopic ifAbsent: [^0].
			handlers do: [ :handler | handler value: aPayload ].
			^handlers size
		]

	]

]
