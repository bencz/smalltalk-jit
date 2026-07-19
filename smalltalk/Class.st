Class := ClassDescription [

	| name comment category classVariables namespace |


	"accessing"

	name [
		^name
	]


	namespace [
		^namespace
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
