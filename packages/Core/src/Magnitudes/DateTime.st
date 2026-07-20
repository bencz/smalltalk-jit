"A point on the UTC timeline. Internally it is a single integer of absolute
 milliseconds since 1970-01-01 00:00:00 UTC (milliSeconds), which is ALWAYS
 the true instant, plus a presentation-only offsetSeconds (seconds east of
 UTC, default 0 = UTC). Ordering, equality and subtraction look ONLY at
 milliSeconds; offsetSeconds only shifts how the calendar/time components are
 rendered (year/month/day/hour/minute/second decompose milliSeconds +
 offsetSeconds * 1000). With the default offset of 0 the component accessors
 answer plain UTC, exactly as before."

DateTime := Magnitude [

	| milliSeconds offsetSeconds MillisecondsPerDay DaysIn4Years DaysIn100Years
		DaysIn400Years Days1970To2000 DaysOffset YearsOffset
		MonthNames MonthAbbreviations WeekdayNames WeekdayAbbreviations DaysPerMonth |

	"--- instance creation ---"

	class now [
		^self new initializeMilliSeconds: self currentMicroTime // 1000
	]


	class nowLocal [
		"Same instant as now, but tagged with the machine's current UTC offset so
		 the component accessors render local wall-clock time."
		| dt |

		dt := self now.
		^dt setMilliSeconds: dt asMilliseconds
			offsetSeconds: (self localUtcOffsetAt: dt asUnixTime)
	]


	class today [
		"Midnight (00:00:00 UTC) of the current UTC day."
		| ms |

		ms := self currentMicroTime // 1000.
		^self fromMilliseconds: (ms // MillisecondsPerDay) * MillisecondsPerDay
	]


	class fromMilliseconds: anInteger [
		^self new initializeMilliSeconds: anInteger
	]


	class fromUnixTime: seconds [
		"seconds since the 1970 epoch."
		^self fromMilliseconds: seconds * 1000
	]


	class year: y month: m day: d [
		^self year: y month: m day: d hour: 0 minute: 0 second: 0 millisecond: 0
	]


	class year: y month: m day: d hour: h minute: mi second: s [
		^self year: y month: m day: d hour: h minute: mi second: s millisecond: 0
	]


	class year: y month: m day: d hour: h minute: mi second: s millisecond: ms [
		| days |

		days := self daysFromCivilYear: y month: m day: d.
		^self fromMilliseconds:
			(days * MillisecondsPerDay)
				+ (h * 3600000) + (mi * 60000) + (s * 1000) + ms
	]


	class initialize [
		MillisecondsPerDay := 24 * 60 * 60 * 1000.
		DaysIn4Years := 4 * 365 + 1.
		DaysIn100Years := 25 * DaysIn4Years - 1.
		DaysIn400Years := 4 * DaysIn100Years + 1.
		Days1970To2000 := 30 * 365 + 7.
		DaysOffset := (1000 * DaysIn400Years) + (5 * DaysIn400Years) - Days1970To2000.
		YearsOffset := 400000.
		MonthNames := #('January' 'February' 'March' 'April' 'May' 'June'
			'July' 'August' 'September' 'October' 'November' 'December').
		MonthAbbreviations := #('Jan' 'Feb' 'Mar' 'Apr' 'May' 'Jun'
			'Jul' 'Aug' 'Sep' 'Oct' 'Nov' 'Dec').
		WeekdayNames := #('Sunday' 'Monday' 'Tuesday' 'Wednesday'
			'Thursday' 'Friday' 'Saturday').
		WeekdayAbbreviations := #('Sun' 'Mon' 'Tue' 'Wed' 'Thu' 'Fri' 'Sat').
		DaysPerMonth := #(31 28 31 30 31 30 31 31 30 31 30 31).
	]


	class currentMicroTime [
		<primitive: CurrentMicroTimePrimitive>
	]


	class localUtcOffsetAt: epochSeconds [
		"Seconds east of UTC in effect at the given epoch second (DST-aware)."
		<primitive: LocalUtcOffsetPrimitive>
	]


	"--- calendar helpers (class side) ---"

	class daysFromCivilYear: y month: m day: d [
		"Civil-to-days, Howard Hinnant's public-domain algorithm, the exact
		 inverse of computeDate. Answers days since 1970-01-01 on a March-based
		 400-year era so the leap day lands at the era's end. Correct for every
		 date, leap years and 400-year boundaries included, and for pre-epoch
		 dates (floored // yields the right era for negative years)."
		| yy era yoe doy doe monthShift |

		yy := y.
		m <= 2 ifTrue: [yy := yy - 1].
		era := yy // 400.
		yoe := yy - (era * 400).
		monthShift := m > 2 ifTrue: [m - 3] ifFalse: [m + 9].
		doy := (((153 * monthShift) + 2) // 5) + d - 1.
		doe := (yoe * 365) + (yoe // 4) - (yoe // 100) + doy.
		^(era * 146097) + doe - 719468
	]


	class isLeapYear: y [
		^(y \\ 4 = 0) and: [(y \\ 100 ~= 0) or: [y \\ 400 = 0]]
	]


	class daysInMonth: m year: y [
		m = 2 ifTrue: [^(self isLeapYear: y) ifTrue: [29] ifFalse: [28]].
		^DaysPerMonth at: m
	]


	class monthName: m [
		^MonthNames at: m
	]


	class monthAbbreviation: m [
		^MonthAbbreviations at: m
	]


	class weekdayName: dow [
		"dow is 1=Sunday .. 7=Saturday."
		^WeekdayNames at: dow
	]


	class weekdayAbbreviation: dow [
		^WeekdayAbbreviations at: dow
	]


	"--- ISO 8601 / string parsing (class side) ---"

	class fromString: aString [
		^self readFrom: aString
	]


	class readFrom: aString [
		"Accept 'YYYY-MM-DD', 'YYYY-MM-DDTHH:MM:SS', 'YYYY-MM-DDTHH:MM:SS.mmm'
		 with an optional 'Z' or '+HH:MM' / '-HH:MM' zone suffix. A zone offset is
		 applied by converting the parsed local wall-clock time back to UTC; the
		 offset is remembered for presentation. No suffix is treated as UTC."
		| str pos len readInt year month day hour minute second milli
		  offset offSign offHour offMin fVal fDigits ch localMs |

		str := aString trimSeparators.
		pos := 1.
		len := str size.
		readInt := [ | v |
			v := 0.
			[ pos <= len and: [(str at: pos) isDigit] ] whileTrue: [
				v := (v * 10) + (str at: pos) digitValue.
				pos := pos + 1 ].
			v ].

		year := readInt value.
		(pos <= len and: [(str at: pos) = $-]) ifTrue: [pos := pos + 1].
		month := readInt value.
		(pos <= len and: [(str at: pos) = $-]) ifTrue: [pos := pos + 1].
		day := readInt value.
		hour := 0. minute := 0. second := 0. milli := 0. offset := 0.

		(pos <= len and: [((str at: pos) = $T) or: [(str at: pos) = $ ]]) ifTrue: [
			pos := pos + 1.
			hour := readInt value.
			(pos <= len and: [(str at: pos) = $:]) ifTrue: [pos := pos + 1].
			minute := readInt value.
			(pos <= len and: [(str at: pos) = $:]) ifTrue: [pos := pos + 1].
			second := readInt value.
			(pos <= len and: [(str at: pos) = $.]) ifTrue: [
				pos := pos + 1.
				fVal := 0. fDigits := 0.
				[ pos <= len and: [(str at: pos) isDigit] ] whileTrue: [
					fDigits < 3 ifTrue: [
						fVal := (fVal * 10) + (str at: pos) digitValue.
						fDigits := fDigits + 1 ].
					pos := pos + 1 ].
				[ fDigits < 3 ] whileTrue: [ fVal := fVal * 10. fDigits := fDigits + 1 ].
				milli := fVal ] ].

		pos <= len ifTrue: [
			ch := str at: pos.
			ch = $Z ifTrue: [ pos := pos + 1. offset := 0 ].
			((ch = $+) or: [ch = $-]) ifTrue: [
				offSign := ch = $- ifTrue: [-1] ifFalse: [1].
				pos := pos + 1.
				offHour := readInt value.
				(pos <= len and: [(str at: pos) = $:]) ifTrue: [pos := pos + 1].
				offMin := (pos <= len and: [(str at: pos) isDigit])
					ifTrue: [readInt value] ifFalse: [0].
				offset := offSign * ((offHour * 3600) + (offMin * 60)) ] ].

		localMs := ((self daysFromCivilYear: year month: month day: day) * MillisecondsPerDay)
			+ (hour * 3600000) + (minute * 60000) + (second * 1000) + milli.
		^self new setMilliSeconds: localMs - (offset * 1000) offsetSeconds: offset
	]


	"--- zero-padded formatting helpers (class side) ---"

	class twoDigits: n [
		^n < 10 ifTrue: ['0', n printString] ifFalse: [n printString]
	]


	class threeDigits: n [
		n < 10 ifTrue: [^'00', n printString].
		n < 100 ifTrue: [^'0', n printString].
		^n printString
	]


	class fourDigits: n [
		| s |

		n < 0 ifTrue: [^n printString].
		s := n printString.
		[s size < 4] whileTrue: [s := '0', s].
		^s
	]


	"--- initialization ---"

	initializeMilliSeconds: anInteger [
		milliSeconds := anInteger.
		offsetSeconds := 0.
		^self
	]


	setMilliSeconds: ms offsetSeconds: off [
		milliSeconds := ms.
		offsetSeconds := off.
		^self
	]


	"--- accessing ---"

	localMilliSeconds [
		"Absolute UTC milliSeconds shifted by the presentation offset. All
		 component accessors decompose this; with offset 0 it equals milliSeconds."
		^milliSeconds + (offsetSeconds * 1000)
	]


	asMilliseconds [
		^milliSeconds
	]


	asUnixTime [
		^milliSeconds // 1000
	]


	offsetSeconds [
		^offsetSeconds
	]


	offset [
		^Duration seconds: offsetSeconds
	]


	millisecond [
		^self localMilliSeconds \\ 1000
	]


	second [
		^self localMilliSeconds // 1000 \\ 60
	]


	minute [
		^self localMilliSeconds // (1000 * 60) \\ 60
	]


	hour [
		^self localMilliSeconds // (1000 * 60 * 60) \\ 24
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


	secondsSinceMidnight [
		^(self localMilliSeconds \\ MillisecondsPerDay) // 1000
	]


	computeDate [
		"Days-to-civil (Howard Hinnant's public-domain algorithm), on a March-based
		 400-year era so the leap day falls at the era's end. Correct for every
		 date, including Jan 1 of leap years and 400-year boundaries, and for
		 pre-epoch dates (floored // and \\). Answers {year. month. day}; the
		 hour/minute/second accessors handle the time of day separately.
		 The prior hand-rolled -1/+1 version rendered Jan 1 of every leap year as
		 Dec 31 of the previous year; this version renders 2000-01-01 correctly
		 (946684800000 ms decomposes to year 2000, month 1, day 1)."
		| z era doe yoe y doy mp d m |

		z := (self localMilliSeconds // MillisecondsPerDay) + 719468.
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


	"--- calendar (instance side) ---"

	dayOfWeek [
		"1=Sunday, 2=Monday, ... 7=Saturday. 1970-01-01 (day 0) was a Thursday."
		| days |

		days := self localMilliSeconds // MillisecondsPerDay.
		^((days + 4) \\ 7) + 1
	]


	dayOfYear [
		"1 for January 1st."
		| c |

		c := self computeDate.
		^(DateTime daysFromCivilYear: (c at: 1) month: (c at: 2) day: (c at: 3))
			- (DateTime daysFromCivilYear: (c at: 1) month: 1 day: 1) + 1
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


	monthAbbreviation [
		^DateTime monthAbbreviation: self month
	]


	weekdayName [
		^DateTime weekdayName: self dayOfWeek
	]


	weekdayAbbreviation [
		^DateTime weekdayAbbreviation: self dayOfWeek
	]


	"--- converting ---"

	asDate [
		^Date fromDays: (self localMilliSeconds // MillisecondsPerDay)
	]


	asTime [
		^Time fromMilliseconds: (self localMilliSeconds \\ MillisecondsPerDay)
	]


	asUtc [
		^DateTime new setMilliSeconds: milliSeconds offsetSeconds: 0
	]


	asLocal [
		^DateTime new setMilliSeconds: milliSeconds
			offsetSeconds: (DateTime localUtcOffsetAt: self asUnixTime)
	]


	"--- comparing ---"

	< aDateTime [
		^milliSeconds < aDateTime asMilliseconds
	]


	= aDateTime [
		^(aDateTime isKindOf: DateTime) and: [milliSeconds = aDateTime asMilliseconds]
	]


	hash [
		^milliSeconds hash
	]


	"--- arithmetic ---"

	+ aDuration [
		^self addMilliseconds: aDuration asMilliseconds
	]


	- aValue [
		(aValue isKindOf: DateTime) ifTrue: [
			^Duration milliseconds: milliSeconds - aValue asMilliseconds].
		(aValue isKindOf: Duration) ifTrue: [
			^self addMilliseconds: aValue asMilliseconds negated].
		^self error: 'DateTime - expects a DateTime or a Duration'
	]


	addMilliseconds: n [
		^DateTime new setMilliSeconds: milliSeconds + n offsetSeconds: offsetSeconds
	]


	addSeconds: n [
		^self addMilliseconds: n * 1000
	]


	addMinutes: n [
		^self addMilliseconds: n * 60000
	]


	addHours: n [
		^self addMilliseconds: n * 3600000
	]


	addDays: n [
		^self addMilliseconds: n * MillisecondsPerDay
	]


	addWeeks: n [
		^self addMilliseconds: n * 7 * MillisecondsPerDay
	]


	addMonths: n [
		"Calendar arithmetic on the local components: add n to the month field and
		 clamp the day of month to the last day of the destination month (31 Jan +
		 1 month = 28/29 Feb). Time of day and offset are preserved."
		| c y m d total newY newM dim tod |

		c := self computeDate.
		y := c at: 1. m := c at: 2. d := c at: 3.
		total := (y * 12) + (m - 1) + n.
		newY := total // 12.
		newM := (total \\ 12) + 1.
		dim := DateTime daysInMonth: newM year: newY.
		d > dim ifTrue: [d := dim].
		tod := self localMilliSeconds \\ MillisecondsPerDay.
		^DateTime new
			setMilliSeconds:
				((DateTime daysFromCivilYear: newY month: newM day: d) * MillisecondsPerDay)
					+ tod - (offsetSeconds * 1000)
			offsetSeconds: offsetSeconds
	]


	addYears: n [
		^self addMonths: n * 12
	]


	"--- printing ---"

	printOn: aStream [
		"Legacy human-readable rendering: local date and time separated by a space,
		 with no zone marker (kept stable so existing prints do not change)."
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


	printIso8601 [
		"ISO 8601 with the 'T' separator and a zone marker: 'Z' when the offset is
		 UTC, else '+HH:MM' / '-HH:MM'. Milliseconds are printed only when nonzero."
		| c result |

		c := self computeDate.
		result := (DateTime fourDigits: (c at: 1)), '-',
			(DateTime twoDigits: (c at: 2)), '-', (DateTime twoDigits: (c at: 3)),
			'T', (DateTime twoDigits: self hour), ':',
			(DateTime twoDigits: self minute), ':', (DateTime twoDigits: self second).
		self millisecond > 0 ifTrue: [
			result := result, '.', (DateTime threeDigits: self millisecond) ].
		offsetSeconds = 0
			ifTrue: [ result := result, 'Z' ]
			ifFalse: [ | absOff sign |
				sign := offsetSeconds < 0 ifTrue: ['-'] ifFalse: ['+'].
				absOff := offsetSeconds abs.
				result := result, sign,
					(DateTime twoDigits: absOff // 3600), ':',
					(DateTime twoDigits: (absOff \\ 3600) // 60) ].
		^result
	]


	printFormat: pattern [
		"Render according to a pattern built from the tokens yyyy yy MM M dd d HH
		 mm ss (any other run of characters is copied literally)."
		| result i len ch j |

		result := ''.
		i := 1.
		len := pattern size.
		[i <= len] whileTrue: [
			ch := pattern at: i.
			(ch isLetter and: [self isFormatLetter: ch])
				ifTrue: [
					j := i.
					[j <= len and: [(pattern at: j) = ch]] whileTrue: [j := j + 1].
					result := result, (self expandFormatToken: (pattern copyFrom: i to: j - 1)).
					i := j ]
				ifFalse: [
					result := result, (String with: ch).
					i := i + 1 ] ].
		^result
	]


	isFormatLetter: ch [
		^(ch = $y) or: [(ch = $M) or: [(ch = $d)
			or: [(ch = $H) or: [(ch = $m) or: [ch = $s]]]]]
	]


	expandFormatToken: token [
		token = 'yyyy' ifTrue: [^DateTime fourDigits: self year].
		token = 'yy' ifTrue: [^DateTime twoDigits: self year \\ 100].
		token = 'MM' ifTrue: [^DateTime twoDigits: self month].
		token = 'M' ifTrue: [^self month printString].
		token = 'dd' ifTrue: [^DateTime twoDigits: self day].
		token = 'd' ifTrue: [^self day printString].
		token = 'HH' ifTrue: [^DateTime twoDigits: self hour].
		token = 'mm' ifTrue: [^DateTime twoDigits: self minute].
		token = 'ss' ifTrue: [^DateTime twoDigits: self second].
		^token
	]

]
