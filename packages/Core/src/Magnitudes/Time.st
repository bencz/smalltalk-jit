"A time of day with no calendar date. Held as an integer count of
 milliseconds since midnight (0 .. 86399999)."

Time := Magnitude [

	| millis |


	"--- instance creation ---"

	class fromMilliseconds: anInteger [
		^self new setMilliseconds: anInteger
	]


	class hour: h minute: m second: s [
		^self fromMilliseconds: (h * 3600000) + (m * 60000) + (s * 1000)
	]


	class now [
		^self fromMilliseconds: (DateTime now asMilliseconds \\ 86400000)
	]


	setMilliseconds: anInteger [
		millis := anInteger.
		^self
	]


	"--- accessing ---"

	asMilliseconds [
		^millis
	]


	hour [
		^millis // 3600000
	]


	minute [
		^(millis // 60000) \\ 60
	]


	second [
		^(millis // 1000) \\ 60
	]


	millisecond [
		^millis \\ 1000
	]


	asDuration [
		^Duration milliseconds: millis
	]


	"--- comparing ---"

	< aTime [
		^millis < aTime asMilliseconds
	]


	= aTime [
		^(aTime isKindOf: Time) and: [millis = aTime asMilliseconds]
	]


	hash [
		^millis hash
	]


	"--- printing ---"

	printOn: aStream [
		self hour < 10 ifTrue: [aStream nextPut: $0].
		self hour printOn: aStream.
		aStream nextPut: $:.
		self minute < 10 ifTrue: [aStream nextPut: $0].
		self minute printOn: aStream.
		aStream nextPut: $:.
		self second < 10 ifTrue: [aStream nextPut: $0].
		self second printOn: aStream.
	]

]
