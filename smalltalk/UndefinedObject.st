UndefinedObject := Object [

	"testing"

	isNil [
		^true
	]


	notNil [
		^false
	]


	"nil testing"

	ifNil: aBlock [
		^aBlock value
	]


	ifNotNil: aBlock [
		^nil
	]


	ifNil: nilBlock ifNotNil: notNilBlock [
		^nilBlock value
	]


	ifNotNil: notNilBlock ifNil: nilBlock [
		^nilBlock value
	]


	"copying"

	copy [
		^self
	]


	"printing"

	printOn: aStream [
		aStream nextPutAll: 'nil'.
	]

]
