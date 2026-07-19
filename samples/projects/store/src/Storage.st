"Persistence-shaped layer: a keyed repository with block queries, an
 inventory that RAISES a custom exception (a domain-specific Error subclass
 living in this namespace) when a reservation cannot be honored."
Storage := Namespace [

	OutOfStockError := Error [

		| sku requested available |

		class sku: aString requested: wanted available: have [
			^self new setSku: aString requested: wanted available: have
		]

		setSku: aString requested: wanted available: have [
			sku := aString.
			requested := wanted.
			available := have.
			^self
		]

		sku [ ^sku ]
		requested [ ^requested ]
		available [ ^available ]

		messageText [
			^'out of stock: ', sku, ' (requested ', requested printString,
				', available ', available printString, ')'
		]

	]

	Repository := Object [

		| items |

		class new [
			^self basicNew initRepository
		]

		initRepository [
			items := Dictionary new.
			^self
		]

		at: aKey put: anObject [
			^items at: aKey put: anObject
		]

		at: aKey [
			^items at: aKey
		]

		size [
			^items size
		]

		do: aBlock [
			items keysAndValuesDo: [ :key :each | aBlock value: each ].
		]

		values [
			| out |
			out := OrderedCollection new.
			self do: [ :each | out add: each ].
			^out
		]

		select: aBlock [
			^self values select: aBlock
		]

	]

	Inventory := Object [

		| stock |

		class new [
			^self basicNew initInventory
		]

		initInventory [
			stock := Dictionary new.
			^self
		]

		stock: aSku with: aQuantity [
			stock at: aSku put: aQuantity.
		]

		availableOf: aSku [
			^stock at: aSku ifAbsent: [0]
		]

		reserve: aSku quantity: aQuantity [
			| have |
			have := self availableOf: aSku.
			have < aQuantity ifTrue: [
				(OutOfStockError sku: aSku requested: aQuantity available: have) signal].
			stock at: aSku put: have - aQuantity.
		]

	]

]
