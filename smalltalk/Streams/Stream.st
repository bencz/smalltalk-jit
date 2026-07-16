Stream := Iterator [

	"accessing"

	next [
		SubClassResponsibility signal.
	]


	next: anInteger [
		SubClassResponsibility signal.
	]


	nextMatchFor: anObject [
		^self next = anObject
	]


	contents [
		SubClassResponsibility signal.
	]


	nextPut: anObject [
		SubClassResponsibility signal.
	]


	nextPutAll: aCollection [
		aCollection do: [ :v | self nextPut: v].
		^aCollection
	]


	next: anInteger put: anObject [
		anInteger timesRepeat: [self nextPut: anObject].
	]


	show: aString [
		^self nextPutAll: aString.
	]


	showCr: aString [
		"Terminates the line with lf, not cr: on this platform a newline is lf,
		 and #cr below emits a literal carriage return."
		self nextPutAll: aString.
		self lf.
		^aString
	]


	cr [
		self nextPut: Character cr.
	]


	lf [
		self nextPut: Character lf.
	]


	space [
		self nextPut: Character space.
	]


	tab [
		self nextPut: Character tab.
	]


	tab: anInteger [
		self next: anInteger put: Character tab.
	]


	"testing"

	atEnd [
		SubClassResponsibility signal.
	]


	"enumerating"

	do: aBlock [
		[self atEnd] whileFalse: [aBlock value: self next].
	]

]
