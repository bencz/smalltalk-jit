"Magnitude protocol exercised through Number subclasses AND Character.
 Magnitude itself only defines <, and derives >, >=, <=, between:and:, min:,
 max: from it (a subclass provides <). We check the derivations hold, that
 boundaries are inclusive/exclusive as documented, and that ordering is
 transitive across ints, floats, fractions and characters."

[ MagRun := TestRun named: 'Magnitude'.
  "integer ordering and derived operators"
  MagRun check: '1 < 2' that: [ 1 < 2 ].
  MagRun check: '2 > 1 (derived)' that: [ 2 > 1 ].
  MagRun check: '2 >= 2 (derived, boundary)' that: [ 2 >= 2 ].
  MagRun check: '2 <= 2 (derived, boundary)' that: [ 2 <= 2 ].
  MagRun check: '2 <= 3' that: [ 2 <= 3 ].
  MagRun check: '3 >= 2' that: [ 3 >= 2 ].
  MagRun check: 'not 2 < 2 (strict)' that: [ (2 < 2) not ].
  MagRun check: 'not 2 > 2 (strict)' that: [ (2 > 2) not ].
  MagRun check: '1 = 1' that: [ 1 = 1 ].
  MagRun check: '1 ~= 2' that: [ 1 ~= 2 ].
  MagRun check: 'not 1 ~= 1' that: [ (1 ~= 1) not ].
  "min: / max: pick the right side and keep identity on ties"
  MagRun check: 'min 1 2' equals: 1 is: [ 1 min: 2 ].
  MagRun check: 'max 1 2' equals: 2 is: [ 1 max: 2 ].
  MagRun check: 'min 2 1 (reversed)' equals: 1 is: [ 2 min: 1 ].
  MagRun check: 'max 2 1 (reversed)' equals: 2 is: [ 2 max: 1 ].
  MagRun check: 'max on tie' equals: 5 is: [ 5 max: 5 ].
  MagRun check: 'min on tie' equals: 5 is: [ 5 min: 5 ].
  "between:and: is inclusive on both ends"
  MagRun check: '3 between 1 and 5' that: [ 3 between: 1 and: 5 ].
  MagRun check: '1 between 1 and 5 (low boundary)' that: [ 1 between: 1 and: 5 ].
  MagRun check: '5 between 1 and 5 (high boundary)' that: [ 5 between: 1 and: 5 ].
  MagRun check: 'not 0 between 1 and 5' that: [ (0 between: 1 and: 5) not ].
  MagRun check: 'not 6 between 1 and 5' that: [ (6 between: 1 and: 5) not ].
  "transitivity a<b & b<c -> a<c"
  MagRun check: 'transitivity ints' that: [ (1 < 2) and: [ (2 < 3) and: [ 1 < 3 ] ] ].
]

[ "floats, fractions and cross-type Magnitude comparisons"
  MagRun check: '1.5 < 2.5' that: [ 1.5 < 2.5 ].
  MagRun check: '2.5 > 1.5 (derived)' that: [ 2.5 > 1.5 ].
  MagRun check: '2.0 <= 2.0 (derived boundary)' that: [ 2.0 <= 2.0 ].
  MagRun check: 'max floats' equals: 3.0 is: [ 2.0 max: 3.0 ].
  MagRun check: 'min floats' equals: 2.0 is: [ 2.0 min: 3.0 ].
  MagRun check: '2.5 between 2.0 and 3.0' that: [ 2.5 between: 2.0 and: 3.0 ].
  MagRun check: 'transitivity floats' that: [ (1.0 < 1.5) and: [ (1.5 < 2.0) and: [ 1.0 < 2.0 ] ] ].
  MagRun check: '(1/3) < (1/2)' that: [ (1/3) < (1/2) ].
  MagRun check: 'max fractions' equals: (2/3) is: [ (1/2) max: (2/3) ].
  MagRun check: 'min fractions' equals: (1/2) is: [ (1/2) min: (2/3) ].
  MagRun check: '(1/2) between 0 and 1' that: [ (1/2) between: 0 and: 1 ].
  "mixed-type magnitude comparisons coerce and still order correctly"
  MagRun check: 'max int/float' equals: 2.5 is: [ 1 max: 2.5 ].
  MagRun check: 'min float/int' equals: 1 is: [ 2.5 min: 1 ].
  MagRun check: '1 between (1/2) and 2' that: [ 1 between: (1/2) and: 2 ].
  MagRun check: '(1/2) < 1' that: [ (1/2) < 1 ].
  MagRun check: '3 > (1/2)' that: [ 3 > (1/2) ].
  MagRun check: 'value equality (1/2) = (2/4)' that: [ (1/2) = (2/4) ].
  MagRun check: 'not (1/2) ~= (2/4)' that: [ ((1/2) ~= (2/4)) not ].
]

[ "Character IS a Magnitude: comparison is by code point"
  MagRun check: '$a < $b' that: [ $a < $b ].
  MagRun check: '$b > $a (derived)' that: [ $b > $a ].
  MagRun check: '$a <= $a (derived boundary)' that: [ $a <= $a ].
  MagRun check: '$a >= $a (derived boundary)' that: [ $a >= $a ].
  MagRun check: 'not $b < $a' that: [ ($b < $a) not ].
  MagRun check: '$0 < $9' that: [ $0 < $9 ].
  MagRun check: 'uppercase before lowercase ($A < $a)' that: [ $A < $a ].
  MagRun check: 'char max' equals: $z is: [ $a max: $z ].
  MagRun check: 'char min' equals: $a is: [ $a min: $z ].
  MagRun check: '$m between $a and $z' that: [ $m between: $a and: $z ].
  MagRun check: 'not $A between $a and $z' that: [ ($A between: $a and: $z) not ].
  MagRun check: 'char identity equality $a = $a' that: [ $a = $a ].
  MagRun check: 'char == $a' that: [ $a == $a ].
  MagRun check: 'char $a ~= $b' that: [ $a ~= $b ].
  MagRun check: 'not $a = $b' that: [ ($a = $b) not ].
  MagRun check: 'char transitivity' that: [ ($a < $m) and: [ ($m < $z) and: [ $a < $z ] ] ].
  ^MagRun report
]
