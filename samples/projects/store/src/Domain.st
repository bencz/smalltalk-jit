"The domain model. Money is a proper value object (integer cents inside,
 arithmetic and comparison outside), and the namespace closes with an
 Integer extension so `1990 cents` reads like the money literal the language
 does not need: the extension lands on the CORE Integer, but its body
 resolves Money HERE, in the extending namespace."
Domain := Namespace [

	Money := Object [

		| cents |

		class cents: anInteger [
			^self basicNew setCents: anInteger
		]

		class zero [
			^self cents: 0
		]

		setCents: anInteger [
			cents := anInteger.
			^self
		]

		cents [
			^cents
		]

		+ aMoney [
			^Money cents: cents + aMoney cents
		]

		- aMoney [
			^Money cents: cents - aMoney cents
		]

		* anInteger [
			^Money cents: cents * anInteger
		]

		percent: aRate [
			"aRate percent of the receiver, rounded to the cent"
			^Money cents: (cents * aRate + 50) // 100
		]

		< aMoney [
			^cents < aMoney cents
		]

		agreesWith: aMoney [
			^cents = aMoney cents
		]

		printOn: aStream [
			| frac |
			aStream nextPutAll: '$ '.
			(cents // 100) printOn: aStream.
			aStream nextPutAll: '.'.
			frac := cents \\ 100.
			frac < 10 ifTrue: [aStream nextPutAll: '0'].
			frac printOn: aStream.
		]

	]

	Product := Object [

		| sku name category price |

		class sku: aString name: aName category: aSymbol price: aMoney [
			^self basicNew setSku: aString name: aName category: aSymbol price: aMoney
		]

		setSku: aString name: aName category: aSymbol price: aMoney [
			sku := aString.
			name := aName.
			category := aSymbol.
			price := aMoney.
			^self
		]

		sku [ ^sku ]
		name [ ^name ]
		category [ ^category ]
		price [ ^price ]

		printOn: aStream [
			aStream nextPutAll: name; nextPutAll: ' ('; nextPutAll: sku.
			aStream nextPutAll: ', '; nextPutAll: category; nextPutAll: ') '.
			price printOn: aStream.
		]

	]

	Customer := Object [

		| name |

		class named: aString [
			^self basicNew setName: aString
		]

		setName: aString [
			name := aString.
			^self
		]

		name [ ^name ]

	]

	OrderLine := Object [

		| product quantity |

		class product: aProduct quantity: anInteger [
			^self basicNew setProduct: aProduct quantity: anInteger
		]

		setProduct: aProduct quantity: anInteger [
			product := aProduct.
			quantity := anInteger.
			^self
		]

		product [ ^product ]
		quantity [ ^quantity ]

		subtotal [
			^product price * quantity
		]

	]

	Order := Object [

		| id customer lines |

		class id: anInteger customer: aCustomer [
			^self basicNew setId: anInteger customer: aCustomer
		]

		setId: anInteger customer: aCustomer [
			id := anInteger.
			customer := aCustomer.
			lines := OrderedCollection new.
			^self
		]

		add: aProduct quantity: anInteger [
			lines add: (OrderLine product: aProduct quantity: anInteger).
			^self
		]

		id [ ^id ]
		customer [ ^customer ]
		lines [ ^lines ]

		total [
			^lines inject: Money zero into: [ :sum :line | sum + line subtotal ]
		]

		itemCount [
			^lines inject: 0 into: [ :sum :line | sum + line quantity ]
		]

	]

	Integer extend [

		cents [
			^Money cents: self
		]

	]

]
