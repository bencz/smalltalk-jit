MetaClass := ClassDescription [

	"Part of the class mirror: see the note in Behavior.st."

	<shape: ClassShape>

	| instanceClass |


	"accessing"

	instanceClass [
		^instanceClass
	]


	name [
		^instanceClass name
	]


	"printing"

	printOn: aStream [
		instanceClass printOn: aStream.
		aStream nextPutAll: ' class'.
	]

]
