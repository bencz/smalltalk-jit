Class := ClassDescription [

	"The last five tagged fields of RawClass. See the note in Behavior.st."

	<shape: ClassShape>

	| name comment category classVariables namespace |


	"accessing"

	name [
		^name
	]


	namespace [
		"The namespace this class lives in. The class builder writes the field
		 only for a class built into one, so NIL MEANS CORE and is answered as
		 the Core namespace here rather than as nil.

		 It is not stored: a kernel class is built by the C bootstrap, and the
		 Core namespace object is made by Namespace class initialize, which is
		 one of the files that bootstrap is loading. Storing it would give the
		 classes built before that moment nil and the ones after it the object,
		 for no reason a reader could see."
		^namespace ifNil: [Namespaces at: #Core]
	]


	qualifiedName [
		"NsName.ClassName for a class living outside Core; the plain name for
		 kernel classes."
		(namespace isNil or: [namespace == (Namespaces at: #Core)]) ifTrue: [^name].
		^namespace name, '.', name
	]


	classVariables [
		^classVariables
	]


	initialize [

	]


	"printing"

	printOn: aStream [
		aStream nextPutAll: self qualifiedName.
	]

]
