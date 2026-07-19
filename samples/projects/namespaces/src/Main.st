Main := Object [

	class main: args [
		"the namespace objects are plain globals here (bound where they were
		 declared), so members are reached right through them"
		Transcript nextPutAll: ((Greetings at: #Greeter) greet: 'namespaces'); lf.
		Transcript nextPutAll: ((Numbers at: #Stats) mean: #(2 4 6 8)) printString; lf.

		"the Text namespace contributed a core-class extension"
		Transcript nextPutAll: 'smalltalk' shout; lf.

		"same class name, two namespaces, zero ambiguity"
		Transcript nextPutAll: ((Banner at: #Formatter) format: 'banner style'); lf.
		Transcript nextPutAll: ((Plain at: #Formatter) format: 'plain style'); lf.

		"and everything above is REFLECTIVE data: walk the registry and list
		 every non-core namespace with its classes and their methods"
		Transcript lf.
		self listNamespaces.
		^0
	]

	class listNamespaces [
		| names |
		names := OrderedCollection new.
		Namespaces keysDo: [ :name | name == #Core ifFalse: [names add: name] ].
		(names asSortedCollection: [ :a :b | a < b ]) do: [ :name |
			self listNamespace: (Namespaces at: name) ]
	]

	class listNamespace: aNamespace [
		| classNames |
		Transcript nextPutAll: 'namespace '; nextPutAll: aNamespace name; lf.
		classNames := OrderedCollection new.
		aNamespace bindings associationsDo: [ :assoc |
			(assoc value isKindOf: Class) ifTrue: [classNames add: assoc key] ].
		(classNames asSortedCollection: [ :a :b | a < b ]) do: [ :className |
			self listClass: (aNamespace at: className) ]
	]

	class listClass: aClass [
		Transcript nextPutAll: '  '; nextPutAll: aClass name.
		Transcript nextPutAll: ' (superclass: '; nextPutAll: aClass superClass printString.
		Transcript nextPutAll: ')'; lf.
		(self sortedSelectorsOf: aClass class) do: [ :sel |
			Transcript nextPutAll: '    class '; nextPutAll: sel; lf ].
		(self sortedSelectorsOf: aClass) do: [ :sel |
			Transcript nextPutAll: '    '; nextPutAll: sel; lf ]
	]

	class sortedSelectorsOf: aBehavior [
		| selectors |
		selectors := OrderedCollection new.
		aBehavior methodDictionary keysDo: [ :sel | selectors add: sel ].
		^selectors asSortedCollection: [ :a :b | a < b ]
	]

]
