"Invoicing over Domain instances. TaxPolicy is a real polymorphic hierarchy
 (rate is subclassResponsibility); the builder consumes Order/OrderLine
 objects born in the Domain namespace and taxes each line by its product's
 category. Note the two ways of touching another namespace: the explicit
 class fetch `(Domain at: #Money) zero` (Domain resolves here because a
 declared namespace imports its declarant), and the `cents` extension that
 Domain installed on the core Integer."
Billing := Namespace [

	TaxPolicy := Object [

		class for: aCategory [
			aCategory == #food ifTrue: [^FoodTax new].
			aCategory == #luxury ifTrue: [^LuxuryTax new].
			^StandardTax new
		]

		rate [
			^self subclassResponsibility
		]

		taxFor: aMoney [
			^aMoney percent: self rate
		]

	]

	FoodTax := TaxPolicy [
		rate [ ^0 ]
	]

	StandardTax := TaxPolicy [
		rate [ ^10 ]
	]

	LuxuryTax := TaxPolicy [
		rate [ ^25 ]
	]

	Invoice := Object [

		| order subtotal tax |

		class order: anOrder subtotal: aMoney tax: anotherMoney [
			^self basicNew setOrder: anOrder subtotal: aMoney tax: anotherMoney
		]

		setOrder: anOrder subtotal: aMoney tax: anotherMoney [
			order := anOrder.
			subtotal := aMoney.
			tax := anotherMoney.
			^self
		]

		order [ ^order ]
		subtotal [ ^subtotal ]
		tax [ ^tax ]

		total [
			^subtotal + tax
		]

		printOn: aStream [
			aStream nextPutAll: 'invoice #'.
			order id printOn: aStream.
			aStream nextPutAll: ' '; nextPutAll: order customer name.
			aStream nextPutAll: '  subtotal '.
			subtotal printOn: aStream.
			aStream nextPutAll: '  tax '.
			tax printOn: aStream.
			aStream nextPutAll: '  total '.
			self total printOn: aStream.
		]

	]

	InvoiceBuilder := Object [

		buildFor: anOrder [
			| tax |
			tax := (Domain at: #Money) zero.
			anOrder lines do: [ :line |
				tax := tax + ((TaxPolicy for: line product category) taxFor: line subtotal) ].
			^Invoice order: anOrder subtotal: anOrder total tax: tax
		]

	]

]
