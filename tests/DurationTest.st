"Duration: an integer number of milliseconds (possibly negative). Covers the
 named constructors, arithmetic with other Durations, scaling by Numbers,
 component breakdown versus totals, comparison/hashing and ISO 8601 printing."

[ DurRun := TestRun named: 'Duration'.
  "constructors reduce to the right millisecond total"
  DurRun check: 'milliseconds:' equals: 250 is: [ (Duration milliseconds: 250) asMilliseconds ].
  DurRun check: 'seconds:' equals: 5000 is: [ (Duration seconds: 5) asMilliseconds ].
  DurRun check: 'minutes:' equals: 120000 is: [ (Duration minutes: 2) asMilliseconds ].
  DurRun check: 'hours:' equals: 3600000 is: [ (Duration hours: 1) asMilliseconds ].
  DurRun check: 'days:' equals: 86400000 is: [ (Duration days: 1) asMilliseconds ].
  DurRun check: 'weeks:' equals: 604800000 is: [ (Duration weeks: 1) asMilliseconds ].
  DurRun check: 'zero' equals: 0 is: [ Duration zero asMilliseconds ].
  DurRun check: 'days:hours:minutes:seconds:' equals: 93784000 is: [
      (Duration days: 1 hours: 2 minutes: 3 seconds: 4) asMilliseconds ].
  "component accessors break a magnitude down like a clock"
  DurRun check: 'component days' equals: 1 is: [ (Duration days: 1 hours: 2 minutes: 3 seconds: 4) days ].
  DurRun check: 'component hours' equals: 2 is: [ (Duration days: 1 hours: 2 minutes: 3 seconds: 4) hours ].
  DurRun check: 'component minutes' equals: 3 is: [ (Duration days: 1 hours: 2 minutes: 3 seconds: 4) minutes ].
  DurRun check: 'component seconds' equals: 4 is: [ (Duration days: 1 hours: 2 minutes: 3 seconds: 4) seconds ].
  DurRun check: 'component milliseconds' equals: 750 is: [ (Duration milliseconds: 90750) milliseconds ].
  "totals answer the whole magnitude in one unit (exact fractions allowed)"
  DurRun check: 'asSeconds total' equals: 90 is: [ (Duration seconds: 90) asSeconds ].
  DurRun check: 'asMinutes total' equals: 2 is: [ (Duration seconds: 120) asMinutes ].
  DurRun check: 'asHours exact fraction' equals: (3/2) is: [ (Duration minutes: 90) asHours ].
  DurRun check: 'asDays exact fraction' equals: (1/2) is: [ (Duration hours: 12) asDays ].
]

[ "arithmetic"
  DurRun check: 'plus two durations' equals: 4500 is: [
      ((Duration seconds: 3) + (Duration milliseconds: 1500)) asMilliseconds ].
  DurRun check: 'minus two durations' equals: 1500 is: [
      ((Duration seconds: 3) - (Duration milliseconds: 1500)) asMilliseconds ].
  DurRun check: 'times a number' equals: 15000 is: [ ((Duration seconds: 5) * 3) asMilliseconds ].
  DurRun check: 'divided by a number' equals: 2500 is: [ ((Duration seconds: 10) / 4) asMilliseconds ].
  DurRun check: 'negated' equals: 5000 is: [ (Duration seconds: 5) negated asMilliseconds negated ].
  DurRun check: 'abs of negative' equals: 5000 is: [ (Duration seconds: 5) negated abs asMilliseconds ].
  "testing"
  DurRun check: 'zero isZero' that: [ Duration zero isZero ].
  DurRun check: 'positive' that: [ (Duration seconds: 1) positive ].
  DurRun check: 'negative' that: [ (Duration seconds: 1) negated negative ].
  DurRun check: 'not zero positive' that: [ (Duration zero positive) not ].
]

[ "comparison and hashing"
  DurRun check: 'ordering <' that: [ (Duration seconds: 1) < (Duration seconds: 2) ].
  DurRun check: 'ordering > derived' that: [ (Duration seconds: 2) > (Duration seconds: 1) ].
  DurRun check: 'equal by value' that: [ (Duration seconds: 1) = (Duration milliseconds: 1000) ].
  DurRun check: 'not equal' that: [ ((Duration seconds: 1) = (Duration seconds: 2)) not ].
  DurRun check: 'equal share hash' that: [ (Duration seconds: 1) hash = (Duration milliseconds: 1000) hash ].
  DurRun check: 'min:' equals: 1000 is: [ ((Duration seconds: 1) min: (Duration seconds: 2)) asMilliseconds ].
  DurRun check: 'max:' equals: 2000 is: [ ((Duration seconds: 1) max: (Duration seconds: 2)) asMilliseconds ].
  DurRun check: 'usable as a Set element' equals: 1 is: [ | s |
      s := Set new.
      s add: (Duration seconds: 1).
      s add: (Duration milliseconds: 1000).
      s size ].
]

[ "ISO 8601 printing"
  DurRun check: 'print P2DT3H4M5S' equals: 'P2DT3H4M5S' is: [
      (Duration days: 2 hours: 3 minutes: 4 seconds: 5) printString ].
  DurRun check: 'print P1DT2H3M4S' equals: 'P1DT2H3M4S' is: [
      (Duration days: 1 hours: 2 minutes: 3 seconds: 4) printString ].
  DurRun check: 'print zero PT0S' equals: 'PT0S' is: [ Duration zero printString ].
  DurRun check: 'print negative -PT5S' equals: '-PT5S' is: [ (Duration seconds: 5) negated printString ].
  DurRun check: 'print PT1M' equals: 'PT1M' is: [ (Duration minutes: 1) printString ].
  DurRun check: 'print days only P2D' equals: 'P2D' is: [ (Duration days: 2) printString ].
  DurRun check: 'print hours only PT3H' equals: 'PT3H' is: [ (Duration hours: 3) printString ].
  DurRun check: 'print millis PT0.500S' equals: 'PT0.500S' is: [ (Duration milliseconds: 500) printString ].
  DurRun check: 'print negative full -P1DT2H' equals: '-P1DT2H' is: [
      (Duration days: 1 hours: 2 minutes: 0 seconds: 0) negated printString ].
  ^DurRun report
]
