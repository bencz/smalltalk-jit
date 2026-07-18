DateTime := Magnitude [

	| milliSeconds MillisecondsPerDay DaysIn4Years DaysIn100Years DaysIn400Years
		Days1970To2000 DaysOffset YearsOffset |

	class now [
		^self new initializeMilliSeconds: self currentMicroTime // 1000
	]


	class initialize [
		MillisecondsPerDay := 24 * 60 * 60 * 1000.
		DaysIn4Years := 4 * 365 + 1.
		DaysIn100Years := 25 * DaysIn4Years - 1.
		DaysIn400Years := 4 * DaysIn100Years + 1.
		Days1970To2000 := 30 * 365 + 7.
		DaysOffset := (1000 * DaysIn400Years) + (5 * DaysIn400Years) - Days1970To2000.
		YearsOffset := 400000.
	]


	class currentMicroTime [
		<primitive: CurrentMicroTimePrimitive>
	]


	initializeMilliSeconds: anInteger [
		milliSeconds := anInteger.
	]


	"accessing"

	second [
		^milliSeconds // 1000 \\ 60
	]


	minute [
		^milliSeconds // (1000 * 60) \\ 60
	]


	hour [
		^milliSeconds // (1000 * 60 * 60) \\ 24
	]


	day [
		^self computeDate at: 3
	]


	month [
		^self computeDate at: 2
	]


	year [
		^self computeDate at: 1
	]


	computeDate [
		"Days-to-civil (Howard Hinnant's public-domain algorithm), on a March-based
		 400-year era so the leap day falls at the era's end. Correct for every
		 date, including Jan 1 of leap years and 400-year boundaries, and for
		 pre-epoch dates (floored // and \\). Answers {year. month. day}; the
		 hour/minute/second accessors handle the time of day separately.
		 The prior hand-rolled -1/+1 version rendered Jan 1 of every leap year as
		 Dec 31 of the previous year."
		| z era doe yoe y doy mp d m |

		z := (milliSeconds // MillisecondsPerDay) + 719468.
		era := z // 146097.
		doe := z - (era * 146097).
		yoe := (doe - (doe // 1460) + (doe // 36524) - (doe // 146096)) // 365.
		y := yoe + (era * 400).
		doy := doe - ((365 * yoe) + (yoe // 4) - (yoe // 100)).
		mp := ((5 * doy) + 2) // 153.
		d := (doy - (((153 * mp) + 2) // 5)) + 1.
		m := mp < 10 ifTrue: [mp + 3] ifFalse: [mp - 9].
		m <= 2 ifTrue: [y := y + 1].
		^Array with: y with: m with: d
	]


	"printing"

	printOn: aStream [
		| date year month day |

		date := self computeDate.
		year := date at: 1.
		month := date at: 2.
		day := date at: 3.

		year printOn: aStream.
		aStream nextPut: $-.
		month < 10 ifTrue: [aStream nextPut: $0].
		month printOn: aStream.
		aStream nextPut: $-.
		day < 10 ifTrue: [aStream nextPut: $0].
		day printOn: aStream.
		aStream space.

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
