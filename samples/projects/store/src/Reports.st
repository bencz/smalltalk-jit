"Aggregation and text formatting over whatever repository of orders it is
 handed. Revenue rolls up per category into Money (via the Integer cents
 extension this time), best sellers rank by quantity through
 asSortedCollection:, and the table pads its own columns."
Reports := Namespace [

	TextAlign := Object [

		class pad: aString to: aWidth [
			| out |
			out := aString.
			[out size < aWidth] whileTrue: [out := out, ' '].
			^out
		]

		class padLeft: aString to: aWidth [
			| out |
			out := aString.
			[out size < aWidth] whileTrue: [out := ' ', out].
			^out
		]

	]

	SalesReport := Object [

		| orders |

		class over: aRepository [
			^self basicNew setOrders: aRepository
		]

		setOrders: aRepository [
			orders := aRepository.
			^self
		]

		revenueByCategory [
			"Dictionary: category Symbol -> Money"
			| totals |
			totals := Dictionary new.
			orders do: [ :order |
				order lines do: [ :line |
					| category |
					category := line product category.
					totals at: category
						put: ((totals at: category ifAbsent: [0 cents]) + line subtotal) ] ].
			^totals
		]

		unitsPerProduct [
			"Dictionary: product name -> units sold"
			| units |
			units := Dictionary new.
			orders do: [ :order |
				order lines do: [ :line |
					units at: line product name
						put: ((units at: line product name ifAbsent: [0]) + line quantity) ] ].
			^units
		]

		grandTotal [
			^orders values inject: 0 cents into: [ :sum :order | sum + order total ]
		]

		printRevenueTableOn: aStream [
			| totals |
			totals := self revenueByCategory.
			(totals keys asSortedCollection: [ :a :b | a < b ]) do: [ :category |
				aStream nextPutAll: '  '.
				aStream nextPutAll: (TextAlign pad: category to: 10).
				aStream nextPutAll:
					(TextAlign padLeft: (totals at: category) printString to: 10).
				aStream lf ].
		]

		printBestSellersOn: aStream [
			| units ranked |
			units := self unitsPerProduct.
			ranked := units associations asSortedCollection: [ :a :b |
				a value = b value ifTrue: [a key < b key] ifFalse: [a value > b value] ].
			ranked do: [ :assoc |
				aStream nextPutAll: '  '.
				aStream nextPutAll: (TextAlign pad: assoc key to: 18).
				aStream nextPutAll: (TextAlign padLeft: assoc value printString to: 4).
				aStream nextPutAll: ' units'; lf ].
		]

	]

]
