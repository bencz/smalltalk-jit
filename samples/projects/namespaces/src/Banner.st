"Banner and Plain both define a Formatter: same name, two namespaces, two
 fully independent classes."
Banner := Namespace [

	Formatter := Object [

		class format: aString [
			^'== ', aString, ' =='
		]

	]

]
