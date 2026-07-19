Main := Object [

	class main: args [
		Transcript nextPutAll: 'hello from a project'; lf.
		args do: [ :a | Transcript nextPutAll: '  arg: '; nextPutAll: a; lf ].
		^0
	]

]
