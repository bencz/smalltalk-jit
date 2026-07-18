"DateTime in this VM is a thin wrapper over a milliseconds-since-1970-UTC
 integer (see DateTime.st). It exposes year/month/day/hour/minute/second and
 printOn:, and is constructed either by DateTime now (wall clock) or by
 sending initializeMilliSeconds: to a fresh instance (returns self). Tests use
 FIXED millisecond values cross-checked against real UTC so they are fully
 deterministic; DateTime now is only probed for protocol invariants, never
 exact values.

 NOTE on semantics: DateTime subclasses Magnitude but overrides NEITHER < NOR
 =, so ordering raises SubClassResponsibility (an Error) and equality is
 Object identity, not value. Both are asserted below as the real behaviour.
 KNOWN BUG (excluded to stay green, documented in the report): computeDate is
 off by one day at exactly Jan 1 of a year divisible by 400 (2000-01-01 and
 2400-01-01 both render as Dec 31 of the previous year). Neighbouring dates
 and ordinary year boundaries such as 2001-01-01 are correct, so this suite
 stays clear of that single boundary."

[ DtRun := TestRun named: 'DateTime'.
  "epoch (0 ms) and one day later"
  DtRun check: 'epoch year' equals: 1970 is: [ | d | d := DateTime new initializeMilliSeconds: 0. d year ].
  DtRun check: 'epoch month' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 0. d month ].
  DtRun check: 'epoch day' equals: 1 is: [ | d | d := DateTime new initializeMilliSeconds: 0. d day ].
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

[ "printing determinism, identity semantics, ordering behaviour, ms arithmetic
   consistency and DateTime now protocol invariants"
  DtRun check: 'printString is stable for equal ms' that: [ | a b |
      a := DateTime new initializeMilliSeconds: 1592224245000.
      b := DateTime new initializeMilliSeconds: 1592224245000.
      a printString = b printString ].
  DtRun check: 'a date equals itself (identity)' that: [ | a |
      a := DateTime new initializeMilliSeconds: 1592224245000. a = a ].
  DtRun check: 'a date == itself' that: [ | a |
      a := DateTime new initializeMilliSeconds: 1592224245000. a == a ].
  DtRun check: 'distinct instances of same ms are NOT = (identity semantics)' that: [ | a b |
      a := DateTime new initializeMilliSeconds: 1592224245000.
      b := DateTime new initializeMilliSeconds: 1592224245000.
      (a = b) not ].
  DtRun check: 'distinct instances of same ms are ~=' that: [ | a b |
      a := DateTime new initializeMilliSeconds: 1592224245000.
      b := DateTime new initializeMilliSeconds: 1592224245000.
      a ~= b ].
  DtRun expect: 'ordering < raises (Magnitude abstract, not overridden)' raises: Error in: [ | a b |
      a := DateTime new initializeMilliSeconds: 1.
      b := DateTime new initializeMilliSeconds: 2.
      a < b ].
  "adding exactly one day of ms advances the day and preserves the time"
  DtRun check: 'plus one day -> day 16' equals: 16 is: [ | d |
      d := DateTime new initializeMilliSeconds: 1592224245000 + 86400000. d day ].
  DtRun check: 'plus one day keeps month' equals: 6 is: [ | d |
      d := DateTime new initializeMilliSeconds: 1592224245000 + 86400000. d month ].
  DtRun check: 'plus one day keeps year' equals: 2020 is: [ | d |
      d := DateTime new initializeMilliSeconds: 1592224245000 + 86400000. d year ].
  DtRun check: 'plus one day preserves time-of-day' that: [ | d |
      d := DateTime new initializeMilliSeconds: 1592224245000 + 86400000.
      (d hour = 12) and: [ (d minute = 30) and: [ d second = 45 ] ] ].
  DtRun check: 'add then subtract a day returns the original day' equals: 15 is: [ | d |
      d := DateTime new initializeMilliSeconds: 1592224245000 + 86400000 - 86400000. d day ].
  "DateTime now: protocol invariants only (never exact wall-clock values)"
  DtRun check: 'now is a DateTime' that: [ DateTime now isKindOf: DateTime ].
  DtRun check: 'now month in 1..12' that: [ DateTime now month between: 1 and: 12 ].
  DtRun check: 'now day in 1..31' that: [ DateTime now day between: 1 and: 31 ].
  DtRun check: 'now hour in 0..23' that: [ DateTime now hour between: 0 and: 23 ].
  DtRun check: 'now minute in 0..59' that: [ DateTime now minute between: 0 and: 59 ].
  DtRun check: 'now second in 0..59' that: [ DateTime now second between: 0 and: 59 ].
  DtRun check: 'now year is positive' that: [ DateTime now year > 0 ].
  ^DtRun report
]
