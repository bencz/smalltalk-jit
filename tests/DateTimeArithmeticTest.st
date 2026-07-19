"DateTime arithmetic: adding and subtracting Durations, differencing two
 DateTimes into a Duration, calendar-aware addMonths:/addYears: with day
 clamping, component <-> constructor round-trips (including pre-1970 dates),
 and the Unix-time conversions. All fixed millisecond values are cross-checked
 against real UTC so the suite is deterministic and timezone-independent.
 Reference: 2020-06-15T12:30:45Z = 1592224245000 ms."

[ DaRun := TestRun named: 'DateTimeArithmetic'.
  "DateTime + Duration and DateTime - Duration answer DateTimes"
  DaRun check: 'plus one day' equals: 1592310645000 is: [
      ((DateTime new initializeMilliSeconds: 1592224245000) + (Duration days: 1)) asMilliseconds ].
  DaRun check: 'plus a Duration is a DateTime' that: [
      ((DateTime new initializeMilliSeconds: 1592224245000) + (Duration hours: 1)) isKindOf: DateTime ].
  DaRun check: 'minus one hour' equals: 1592220645000 is: [
      ((DateTime new initializeMilliSeconds: 1592224245000) - (Duration hours: 1)) asMilliseconds ].
  DaRun check: 'addSeconds' equals: 1592224255000 is: [
      ((DateTime new initializeMilliSeconds: 1592224245000) addSeconds: 10) asMilliseconds ].
  DaRun check: 'addMinutes' equals: 1592224905000 is: [
      ((DateTime new initializeMilliSeconds: 1592224245000) addMinutes: 11) asMilliseconds ].
  DaRun check: 'addHours' equals: 1592231445000 is: [
      ((DateTime new initializeMilliSeconds: 1592224245000) addHours: 2) asMilliseconds ].
  DaRun check: 'addWeeks' equals: 1592829045000 is: [
      ((DateTime new initializeMilliSeconds: 1592224245000) addWeeks: 1) asMilliseconds ].
  "DateTime - DateTime answers a Duration"
  DaRun check: 'diff is a Duration' that: [
      ((DateTime new initializeMilliSeconds: 1592310645000) - (DateTime new initializeMilliSeconds: 1592224245000))
          isKindOf: Duration ].
  DaRun check: 'diff of exactly one day' equals: 86400000 is: [
      ((DateTime new initializeMilliSeconds: 1592310645000) - (DateTime new initializeMilliSeconds: 1592224245000))
          asMilliseconds ].
  DaRun check: 'diff negative when earlier minus later' equals: -3600000 is: [
      ((DateTime new initializeMilliSeconds: 1592224245000) - (DateTime new initializeMilliSeconds: 1592227845000))
          asMilliseconds ].
]

[ "calendar addMonths: with end-of-month clamping"
  DaRun check: '31 Jan + 1 month clamps to 28 Feb (2021)' that: [ | d r |
      d := DateTime year: 2021 month: 1 day: 31.
      r := d addMonths: 1.
      (r year = 2021) and: [ (r month = 2) and: [ r day = 28 ] ] ].
  DaRun check: '31 Jan + 1 month clamps to 29 Feb (leap 2020)' that: [ | d r |
      d := DateTime year: 2020 month: 1 day: 31.
      r := d addMonths: 1.
      (r year = 2020) and: [ (r month = 2) and: [ r day = 29 ] ] ].
  DaRun check: '31 Jan + 3 months = 30 Apr' that: [ | r |
      r := (DateTime year: 2021 month: 1 day: 31) addMonths: 3.
      (r month = 4) and: [ r day = 30 ] ].
  DaRun check: 'addMonths across year boundary' that: [ | r |
      r := (DateTime year: 2020 month: 11 day: 15) addMonths: 3.
      (r year = 2021) and: [ (r month = 2) and: [ r day = 15 ] ] ].
  DaRun check: 'addMonths negative goes back' that: [ | r |
      r := (DateTime year: 2020 month: 3 day: 15) addMonths: -4.
      (r year = 2019) and: [ (r month = 11) and: [ r day = 15 ] ] ].
  DaRun check: 'addMonths preserves time of day' that: [ | r |
      r := (DateTime year: 2020 month: 1 day: 15 hour: 8 minute: 9 second: 10) addMonths: 1.
      (r hour = 8) and: [ (r minute = 9) and: [ r second = 10 ] ] ].
  "addYears: over a leap day clamps"
  DaRun check: '29 Feb 2020 + 1 year clamps to 28 Feb 2021' that: [ | r |
      r := (DateTime year: 2020 month: 2 day: 29) addYears: 1.
      (r year = 2021) and: [ (r month = 2) and: [ r day = 28 ] ] ].
  DaRun check: '29 Feb 2020 + 4 years stays 29 Feb 2024' that: [ | r |
      r := (DateTime year: 2020 month: 2 day: 29) addYears: 4.
      (r year = 2024) and: [ (r month = 2) and: [ r day = 29 ] ] ].
]

[ "component <-> constructor round-trips over many dates, including pre-1970"
  DaRun check: 'round-trip 2020-06-15 12:30:45' that: [ | d |
      d := DateTime year: 2020 month: 6 day: 15 hour: 12 minute: 30 second: 45.
      (d year = 2020) and: [ (d month = 6) and: [ (d day = 15)
          and: [ (d hour = 12) and: [ (d minute = 30) and: [ d second = 45 ] ] ] ] ] ].
  DaRun check: 'round-trip pre-epoch 1969-07-20' that: [ | d |
      d := DateTime year: 1969 month: 7 day: 20.
      (d year = 1969) and: [ (d month = 7) and: [ d day = 20 ] ] ].
  DaRun check: 'round-trip 1900-01-01' that: [ | d |
      d := DateTime year: 1900 month: 1 day: 1.
      (d year = 1900) and: [ (d month = 1) and: [ d day = 1 ] ] ].
  DaRun check: 'round-trip 1600-02-29 (leap, 400-boundary era)' that: [ | d |
      d := DateTime year: 1600 month: 2 day: 29.
      (d year = 1600) and: [ (d month = 2) and: [ d day = 29 ] ] ].
  DaRun check: 'round-trip 2000-12-31' that: [ | d |
      d := DateTime year: 2000 month: 12 day: 31.
      (d year = 2000) and: [ (d month = 12) and: [ d day = 31 ] ] ].
  DaRun check: '1969-07-20 milliSeconds is negative' that: [
      (DateTime year: 1969 month: 7 day: 20) asMilliseconds < 0 ].
  "Unix time conversions"
  DaRun check: 'fromUnixTime seconds' equals: 1592224245000 is: [
      (DateTime fromUnixTime: 1592224245) asMilliseconds ].
  DaRun check: 'asUnixTime round-trips' equals: 1592224245 is: [
      (DateTime new initializeMilliSeconds: 1592224245000) asUnixTime ].
  DaRun check: 'fromUnixTime / asUnixTime identity' equals: 1000000000 is: [
      (DateTime fromUnixTime: 1000000000) asUnixTime ].
  DaRun check: 'pre-epoch asUnixTime negative' that: [
      (DateTime year: 1969 month: 7 day: 20) asUnixTime < 0 ].
  ^DaRun report
]
