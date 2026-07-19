"A namespace body can also EXTEND classes; the extension itself lands on the
 core class (visible image-wide), while any class the body defines would stay
 namespaced."
Text := Namespace [

	String extend [

		shout [
			^self asUppercase, '!'
		]

	]

]
