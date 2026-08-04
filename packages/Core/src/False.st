False := Boolean [

	"logical operations"

	& aBoolean [
		^self
	]


	not [
		^true
	]


	| aBoolean [
		^aBoolean
	]


	"controlling"

	ifTrue: aBlock [
		^nil
	]


	ifFalse: aBlock [
		^aBlock value
	]


	ifTrue: aTrueBlock ifFalse: aFalseBlock [
		^aFalseBlock value
	]


	ifFalse: aFalseBlock ifTrue: aTrueBlock [
		^aFalseBlock value
	]


	and: aBlock [
		^self
	]


	or: aBlock [
		^aBlock value
	]


	"controlling, the cold arm"

	mustBeBoolean [
		"See True>>mustBeBoolean: an instance of False that is not the `false`
		 singleton stands for false, and the singleton is what the branch can
		 test by identity."
		^false
	]


	"printing"

	printOn: aStream [
		aStream nextPutAll: 'false'.
	]

]
