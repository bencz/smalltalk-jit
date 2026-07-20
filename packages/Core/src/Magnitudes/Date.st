"A calendar day with no time of day. Held as an integer count of days since
 1970-01-01 (day 0), which may be negative for earlier dates. The heavy
 calendar logic lives on DateTime; Date just delegates to a DateTime pinned at
 midnight UTC of the same day."

Date := Magnitude [

	| days |


	"--- instance creation ---"

	class fromDays: anInteger [
		^self new setDays: anInteger
	]


	class year: y month: m day: d [
		^self fromDays: (DateTime daysFromCivilYear: y month: m day: d)
	]


	class today [
		^self fromDays: (DateTime now asMilliseconds // 86400000)
	]


	setDays: anInteger [
		days := anInteger.
		^self
	]


	"--- accessing ---"

	asDayNumber [
		"Days since 1970-01-01 (day 0)."
		^days
	]


	asDateTime [
		"Midnight UTC of this calendar day."
		^DateTime fromMilliseconds: days * 86400000
	]


	year [
		^self asDateTime year
	]


	month [
		^self asDateTime month
	]


	day [
		^self asDateTime day
	]


	dayOfWeek [
		"1=Sunday .. 7=Saturday."
		^((days + 4) \\ 7) + 1
	]


	dayOfYear [
		^self asDateTime dayOfYear
	]


	isLeapYear [
		^DateTime isLeapYear: self year
	]


	daysInMonth [
		^DateTime daysInMonth: self month year: self year
	]


	monthName [
		^DateTime monthName: self month
	]


	weekdayName [
		^DateTime weekdayName: self dayOfWeek
	]


	"--- arithmetic ---"

	addDays: n [
		^Date fromDays: days + n
	]


	subtractDate: aDate [
		"Number of days between the two calendar days."
		^days - aDate asDayNumber
	]


	"--- comparing ---"

	< aDate [
		^days < aDate asDayNumber
	]


	= aDate [
		^(aDate isKindOf: Date) and: [days = aDate asDayNumber]
	]


	hash [
		^days hash
	]


	"--- printing ---"

	printOn: aStream [
		| c |

		c := self asDateTime computeDate.
		(c at: 1) printOn: aStream.
		aStream nextPut: $-.
		(c at: 2) < 10 ifTrue: [aStream nextPut: $0].
		(c at: 2) printOn: aStream.
		aStream nextPut: $-.
		(c at: 3) < 10 ifTrue: [aStream nextPut: $0].
		(c at: 3) printOn: aStream.
	]

]
