Main := Object [

	class main: args [
		| plainFormatter |
		"classes from imported module namespaces, unqualified"
		Transcript nextPutAll: (Greeter greet: 'namespaces'); lf.
		Transcript nextPutAll: (Stats mean: #(2 4 6 8)) printString; lf.

		"a core-class extension contributed by the Text module"
		Transcript nextPutAll: 'smalltalk' shout; lf.

		"Banner and Plain both export Formatter: the first import wins..."
		Transcript nextPutAll: (Formatter format: 'first import wins'); lf.
		Transcript nextPutAll: Formatter qualifiedName; lf.

		"...and the shadowed one stays reachable reflectively"
		plainFormatter := (Namespaces at: #Plain) at: #Formatter.
		Transcript nextPutAll: (plainFormatter format: 'reflective access'); lf.
		Transcript nextPutAll: plainFormatter qualifiedName; lf.
		^0
	]

]
