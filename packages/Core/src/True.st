True := Boolean [

	"logical operations"

	& aBoolean [
		^aBoolean
	]


	not [
		^false
	]


	| aBoolean [
		^self
	]


	"controlling"

	ifTrue: aBlock [
		^aBlock value
	]


	ifFalse: aBlock [
		^nil
	]


	ifTrue: aTrueBlock ifFalse: aFalseBlock [
		^aTrueBlock value
	]


	ifFalse: aFalseBlock ifTrue: aTrueBlock [
		^aTrueBlock value
	]


	and: aBlock [
		^aBlock value
	]


	or: aBlock [
		^self
	]


	"controlling, the cold arm"

	mustBeBoolean [
		"An inlined conditional reached its cold arm with this receiver, which
		 means the receiver is not the `true` singleton. It is still an instance
		 of True, so it stands for true, and the singleton is what the branch
		 above can test by identity."
		^true
	]


	"printing"

	printOn: aStream [
		aStream nextPutAll: 'true'.
	]

]
