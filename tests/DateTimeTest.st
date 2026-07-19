"DateTime is a thin wrapper over an integer of absolute milliseconds since
 1970-01-01 UTC (see DateTime.st). It exposes year/month/day/hour/minute/second
 plus printOn:, is comparable and hashable by that instant, and now parses and
 renders ISO 8601. Tests use FIXED millisecond values cross-checked against real
 UTC so they are fully deterministic; DateTime now is only probed for protocol
 invariants, never exact values.

 HISTORY: an earlier revision of this suite documented DateTime as having
 IDENTITY equality (no < or = override) and a KNOWN off-by-one BUG at Jan 1 of
 400-divisible years (2000-01-01 rendering as 1999-12-31). Both claims are now
 obsolete: DateTime overrides < = hash (value semantics via milliSeconds), and
 the Hinnant computeDate renders 2000-01-01 correctly. The asserts below were
 INVERTED on purpose to lock in the corrected behaviour, and 2000-01-01 is now
 exercised directly. Evidence for the 400-year verdict: 2000-01-01 00:00:00 UTC
 is exactly 946684800000 ms, which computeDate decomposes to year 2000, month 1,
 day 1 (checked below)."

[ DtRun := TestRun named: 'DateTime'.
  "epoch (0 ms) and one day later"
  DtRun check: 'epoch year' equals: 1970 is: [ | d | d := DateTime new initializeMilliSeconds: 0. d year ].
  DtRun check: 'epoch month' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 0. d month ].
  DtRun check: 'epoch day' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 0. d day ].
  DtRun check: 'epoch is a Thursday (dayOfWeek 5)' equals: 5 is: [ | d | d := DateTime new initializeMilliSeconds: 0. d dayOfWeek ].
  DtRun check: 'epoch weekday name' equals: 'Thursday' is: [ | d | d := DateTime new initializeMilliSeconds: 0. d weekdayName ].
  DtRun check: 'epoch hour/min/sec all 0' that: [ | d | d := DateTime new initializeMilliSeconds: 0.
      (d hour = 0) and: [ (d minute = 0) and: [ d second = 0 ] ] ].
  DtRun check: 'epoch printString' equals: '1970-01-01 00:00:00' is: [ | d | d := DateTime new initializeMilliSeconds: 0. d printString ].
  DtRun check: 'one day later day' equals: 2 is: [ | d | d := DateTime new initializeMilliSeconds: 86400000. d day ].
  DtRun check: 'one day later month' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 86400000. d month ].
  DtRun check: 'one day later year' equals: 1970 is: [ | d | d := DateTime new initializeMilliSeconds: 86400000. d year ].
  "well-known timestamp 1000000000 s = 2001-09-09 01:46:40 UTC"
  DtRun check: '2001 ts year' equals: 2001 is: [ | d | d := DateTime new initializeMilliSeconds: 1000000000000. d year ].
  DtRun check: '2001 ts month' equals: 9 is: [ | d | d := DateTime new initializeMilliSeconds: 1000000000000. d month ].
  DtRun check: '2001 ts day' equals: 9 is: [ | d | d := DateTime new initializeMilliSeconds: 1000000000000. d day ].
  DtRun check: '2001 ts hour' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 1000000000000. d hour ].
  DtRun check: '2001 ts minute' equals: 46 is: [ | d | d := DateTime new initializeMilliSeconds: 1000000000000. d minute ].
  DtRun check: '2001 ts second' equals: 40 is: [ | d | d := DateTime new initializeMilliSeconds: 1000000000000. d second ].
  DtRun check: '2001 ts printString' equals: '2001-09-09 01:46:40' is: [ | d | d := DateTime new initializeMilliSeconds: 1000000000000. d printString ].
]

[ "a full modern datetime, a leap day, a plain year boundary and a pre-epoch date"
  DtRun check: '2020 year' equals: 2020 is: [ | d | d := DateTime new initializeMilliSeconds: 1592224245000. d year ].
  DtRun check: '2020 month' equals: 6 is: [ | d | d := DateTime new initializeMilliSeconds: 1592224245000. d month ].
  DtRun check: '2020 day' equals: 15 is: [ | d | d := DateTime new initializeMilliSeconds: 1592224245000. d day ].
  DtRun check: '2020 hour' equals: 12 is: [ | d | d := DateTime new initializeMilliSeconds: 1592224245000. d hour ].
  DtRun check: '2020 minute' equals: 30 is: [ | d | d := DateTime new initializeMilliSeconds: 1592224245000. d minute ].
  DtRun check: '2020 second' equals: 45 is: [ | d | d := DateTime new initializeMilliSeconds: 1592224245000. d second ].
  DtRun check: '2020 printString' equals: '2020-06-15 12:30:45' is: [ | d | d := DateTime new initializeMilliSeconds: 1592224245000. d printString ].
  "2004-02-29: a real leap day"
  DtRun check: 'leap day year' equals: 2004 is: [ | d | d := DateTime new initializeMilliSeconds: 1078012800000. d year ].
  DtRun check: 'leap day month' equals: 2 is: [ | d | d := DateTime new initializeMilliSeconds: 1078012800000. d month ].
  DtRun check: 'leap day day' equals: 29 is: [ | d | d := DateTime new initializeMilliSeconds: 1078012800000. d day ].
  DtRun check: 'leap day printString' equals: '2004-02-29 00:00:00' is: [ | d | d := DateTime new initializeMilliSeconds: 1078012800000. d printString ].
  "2001-01-01: ordinary (non-400) year boundary, correct"
  DtRun check: 'year boundary year' equals: 2001 is: [ | d | d := DateTime new initializeMilliSeconds: 978307200000. d year ].
  DtRun check: 'year boundary month' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 978307200000. d month ].
  DtRun check: 'year boundary day' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 978307200000. d day ].
  "pre-epoch (negative ms) = 1969-12-31"
  DtRun check: 'pre-epoch year' equals: 1969 is: [ | d | d := DateTime new initializeMilliSeconds: 86400000 negated. d year ].
  DtRun check: 'pre-epoch month' equals: 12 is: [ | d | d := DateTime new initializeMilliSeconds: 86400000 negated. d month ].
  DtRun check: 'pre-epoch day' equals: 31 is: [ | d | d := DateTime new initializeMilliSeconds: 86400000 negated. d day ].
]

[ "The former KNOWN BUG boundary, now exercised directly: Jan 1 of 400-divisible
   years. 2000-01-01 00:00:00 UTC is exactly 946684800000 ms and it IS a Saturday."
  DtRun check: '2000-01-01 ms is 946684800000' equals: 946684800000 is: [
      (DateTime year: 2000 month: 1 day: 1) asMilliseconds ].
  DtRun check: '2000-01-01 year' equals: 2000 is: [ | d | d := DateTime new initializeMilliSeconds: 946684800000. d year ].
  DtRun check: '2000-01-01 month' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 946684800000. d month ].
  DtRun check: '2000-01-01 day' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 946684800000. d day ].
  DtRun check: '2000-01-01 is a Saturday (dayOfWeek 7)' equals: 7 is: [ | d | d := DateTime new initializeMilliSeconds: 946684800000. d dayOfWeek ].
  DtRun check: '2000-01-01 dayOfYear 1' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 946684800000. d dayOfYear ].
  DtRun check: '2000 is a leap year' that: [ DateTime isLeapYear: 2000 ].
  DtRun check: '1900 is NOT a leap year' that: [ (DateTime isLeapYear: 1900) not ].
  DtRun check: '2004 is a leap year' that: [ DateTime isLeapYear: 2004 ].
  DtRun check: '2001 is NOT a leap year' that: [ (DateTime isLeapYear: 2001) not ].
  DtRun check: '2000-02-29 exists (Feb has 29 days)' equals: 29 is: [ DateTime daysInMonth: 2 year: 2000 ].
  DtRun check: '1900-02 has 28 days' equals: 28 is: [ DateTime daysInMonth: 2 year: 1900 ].
  DtRun check: '2000-02-29 round-trips' that: [ | d |
      d := DateTime year: 2000 month: 2 day: 29.
      (d year = 2000) and: [ (d month = 2) and: [ d day = 29 ] ] ].
]

[ "value comparison, ordering and hashing (INVERTED from the old identity asserts)"
  DtRun check: 'printString is stable for equal ms' that: [ | a b |
      a := DateTime new initializeMilliSeconds: 1592224245000.
      b := DateTime new initializeMilliSeconds: 1592224245000.
      a printString = b printString ].
  DtRun check: 'a date equals itself' that: [ | a |
      a := DateTime new initializeMilliSeconds: 1592224245000. a = a ].
  DtRun check: 'a date == itself' that: [ | a |
      a := DateTime new initializeMilliSeconds: 1592224245000. a == a ].
  DtRun check: 'distinct instances of same ms ARE = (value semantics)' that: [ | a b |
      a := DateTime new initializeMilliSeconds: 1592224245000.
      b := DateTime new initializeMilliSeconds: 1592224245000.
      a = b ].
  DtRun check: 'distinct instances of same ms are NOT ~=' that: [ | a b |
      a := DateTime new initializeMilliSeconds: 1592224245000.
      b := DateTime new initializeMilliSeconds: 1592224245000.
      (a ~= b) not ].
  DtRun check: 'ordering < works (Magnitude via milliSeconds)' that: [ | a b |
      a := DateTime new initializeMilliSeconds: 1.
      b := DateTime new initializeMilliSeconds: 2.
      a < b ].
  DtRun check: 'ordering > derived' that: [ | a b |
      a := DateTime new initializeMilliSeconds: 2.
      b := DateTime new initializeMilliSeconds: 1.
      a > b ].
  DtRun check: 'equal instances share hash' that: [ | a b |
      a := DateTime new initializeMilliSeconds: 1592224245000.
      b := DateTime new initializeMilliSeconds: 1592224245000.
      a hash = b hash ].
  DtRun check: 'usable as a Set element (dedups by value)' equals: 1 is: [ | s |
      s := Set new.
      s add: (DateTime new initializeMilliSeconds: 1592224245000).
      s add: (DateTime new initializeMilliSeconds: 1592224245000).
      s size ].
  DtRun check: 'usable as a Dictionary key' equals: 'hit' is: [ | dic |
      dic := Dictionary new.
      dic at: (DateTime new initializeMilliSeconds: 111) put: 'hit'.
      dic at: (DateTime new initializeMilliSeconds: 111) ].
]

[ "ISO 8601 parsing and rendering round-trips"
  DtRun check: 'fromString date only year' equals: 2020 is: [ (DateTime fromString: '2020-06-15') year ].
  DtRun check: 'fromString date only month' equals: 6 is: [ (DateTime fromString: '2020-06-15') month ].
  DtRun check: 'fromString date only day' equals: 15 is: [ (DateTime fromString: '2020-06-15') day ].
  DtRun check: 'fromString with time hour' equals: 12 is: [ (DateTime fromString: '2020-06-15T12:30:45') hour ].
  DtRun check: 'fromString with time minute' equals: 30 is: [ (DateTime fromString: '2020-06-15T12:30:45') minute ].
  DtRun check: 'fromString with time second' equals: 45 is: [ (DateTime fromString: '2020-06-15T12:30:45') second ].
  DtRun check: 'fromString with millis' equals: 123 is: [ (DateTime fromString: '2020-06-15T12:30:45.123') millisecond ].
  DtRun check: 'fromString with Z is UTC instant' equals: 1592224245000 is: [ (DateTime fromString: '2020-06-15T12:30:45Z') asMilliseconds ].
  DtRun check: 'fromString +02:00 shifts to UTC' equals: 1592217045000 is: [ (DateTime fromString: '2020-06-15T12:30:45+02:00') asMilliseconds ].
  DtRun check: 'fromString -05:00 shifts to UTC' equals: 1592242245000 is: [ (DateTime fromString: '2020-06-15T12:30:45-05:00') asMilliseconds ].
  DtRun check: 'printIso8601 round-trips Z' equals: '2020-06-15T12:30:45Z' is: [ (DateTime fromString: '2020-06-15T12:30:45Z') printIso8601 ].
  DtRun check: 'printIso8601 round-trips +02:00' equals: '2020-06-15T12:30:45+02:00' is: [ (DateTime fromString: '2020-06-15T12:30:45+02:00') printIso8601 ].
  DtRun check: 'printIso8601 date-only midnight' equals: '2020-06-15T00:00:00Z' is: [ (DateTime fromString: '2020-06-15') printIso8601 ].
  DtRun check: 'printFormat pattern' equals: '2020-06-15 12:30:45' is: [
      (DateTime fromString: '2020-06-15T12:30:45Z') printFormat: 'yyyy-MM-dd HH:mm:ss' ].
  DtRun check: 'printFormat literal letters' equals: '2020X06X15' is: [
      (DateTime fromString: '2020-06-15') printFormat: 'yyyyXMMXdd' ].
  DtRun check: 'printFormat yy short year' equals: '20', '/06/15' is: [
      (DateTime fromString: '2020-06-15') printFormat: 'yy/MM/dd' ].
  "DateTime now: protocol invariants only (never exact wall-clock values)"
  DtRun check: 'now is a DateTime' that: [ DateTime now isKindOf: DateTime ].
  DtRun check: 'now month in 1..12' that: [ DateTime now month between: 1 and: 12 ].
  DtRun check: 'now day in 1..31' that: [ DateTime now day between: 1 and: 31 ].
  DtRun check: 'now hour in 0..23' that: [ DateTime now hour between: 0 and: 23 ].
  DtRun check: 'now minute in 0..59' that: [ DateTime now minute between: 0 and: 59 ].
  DtRun check: 'now second in 0..59' that: [ DateTime now second between: 0 and: 59 ].
  DtRun check: 'now year is positive' that: [ DateTime now year > 0 ].
  DtRun check: 'nowLocal same instant as now (within a second)' that: [ | a b |
      a := DateTime now. b := DateTime nowLocal.
      (a asMilliseconds - b asMilliseconds) abs <= 1000 ].
  DtRun check: 'asLocal asUtc preserves instant' that: [ | a |
      a := DateTime new initializeMilliSeconds: 1592224245000.
      a asLocal asUtc asMilliseconds = a asMilliseconds ].
  DtRun check: 'offset stays within 16h' that: [ | a |
      a := (DateTime new initializeMilliSeconds: 1592224245000) asLocal.
      a offsetSeconds abs <= (16 * 3600) ].
]

[ "Date: a calendar day with no time of day"
  DtRun check: 'Date components' that: [ | d |
      d := Date year: 2020 month: 6 day: 15.
      (d year = 2020) and: [ (d month = 6) and: [ d day = 15 ] ] ].
  DtRun check: 'Date printString' equals: '2020-06-15' is: [ (Date year: 2020 month: 6 day: 15) printString ].
  DtRun check: 'Date today is a Date' that: [ Date today isKindOf: Date ].
  DtRun check: 'Date dayOfWeek (2020-06-15 is Monday)' equals: 2 is: [ (Date year: 2020 month: 6 day: 15) dayOfWeek ].
  DtRun check: 'Date weekdayName' equals: 'Monday' is: [ (Date year: 2020 month: 6 day: 15) weekdayName ].
  DtRun check: 'Date monthName' equals: 'June' is: [ (Date year: 2020 month: 6 day: 15) monthName ].
  DtRun check: 'Date isLeapYear' that: [ (Date year: 2020 month: 6 day: 15) isLeapYear ].
  DtRun check: 'Date comparison <' that: [ (Date year: 2020 month: 1 day: 1) < (Date year: 2020 month: 1 day: 2) ].
  DtRun check: 'Date value equality' that: [ (Date year: 2020 month: 6 day: 15) = (Date year: 2020 month: 6 day: 15) ].
  DtRun check: 'Date addDays' that: [ | d |
      d := (Date year: 2020 month: 6 day: 15) addDays: 20.
      (d month = 7) and: [ d day = 5 ] ].
  DtRun check: 'Date subtractDate is a day count' equals: 20 is: [
      (Date year: 2020 month: 7 day: 5) subtractDate: (Date year: 2020 month: 6 day: 15) ].
  DtRun check: 'Date asDateTime midnight' equals: 0 is: [ (Date year: 2020 month: 6 day: 15) asDateTime hour ].
  DtRun check: 'DateTime asDate round-trips day' equals: 15 is: [
      (DateTime new initializeMilliSeconds: 1592224245000) asDate day ].
]

[ "Time: a time of day with no calendar date"
  DtRun check: 'Time components' that: [ | t |
      t := Time hour: 12 minute: 30 second: 45.
      (t hour = 12) and: [ (t minute = 30) and: [ t second = 45 ] ] ].
  DtRun check: 'Time printString' equals: '12:30:45' is: [ (Time hour: 12 minute: 30 second: 45) printString ].
  DtRun check: 'Time zero-pads' equals: '01:02:03' is: [ (Time hour: 1 minute: 2 second: 3) printString ].
  DtRun check: 'Time comparison <' that: [ (Time hour: 1 minute: 0 second: 0) < (Time hour: 2 minute: 0 second: 0) ].
  DtRun check: 'Time value equality' that: [ (Time hour: 12 minute: 0 second: 0) = (Time hour: 12 minute: 0 second: 0) ].
  DtRun check: 'Time asDuration' equals: 45045000 is: [ (Time hour: 12 minute: 30 second: 45) asDuration asMilliseconds ].
  DtRun check: 'Time now is a Time' that: [ Time now isKindOf: Time ].
  DtRun check: 'Time now hour in 0..23' that: [ Time now hour between: 0 and: 23 ].
  DtRun check: 'DateTime asTime round-trips' that: [ | t |
      t := (DateTime new initializeMilliSeconds: 1592224245000) asTime.
      (t hour = 12) and: [ (t minute = 30) and: [ t second = 45 ] ] ].
  ^DtRun report
]
