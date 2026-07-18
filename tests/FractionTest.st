"Split into several small top-level blocks: the compiler aborts on very large
single methods, so each block stays well under that limit."

[
	"construction, reduction and demotion to Integer"
	FractionRun := TestRun named: 'Fraction'.

	FractionRun check: 'half class' that: [ (1/2) class == Fraction ].
	FractionRun check: 'reducible class' that: [ (10/2) class == SmallInteger ].
	FractionRun check: 'reducible value' equals: 5 is: [ 10/2 ].
	FractionRun check: 'reduction' that: [ (4/6) = (2/3) ].
	FractionRun check: 'numerator' equals: 2 is: [ (2/3) numerator ].
	FractionRun check: 'denominator' equals: 3 is: [ (2/3) denominator ].
	FractionRun check: 'neg denominator normalizes' that: [ (6/(-4)) = (-3/2) ].
	FractionRun check: 'neg denominator reduces' equals: 2 is: [ (6/(-4)) denominator ].
]


[
	"arithmetic within fractions"
	FractionRun check: 'frac add' that: [ (1/2) + (1/3) = (5/6) ].
	FractionRun check: 'frac sub to zero' equals: 0 is: [ (1/2) - (1/2) ].
	FractionRun check: 'frac sub demotes' that: [ ((1/2) - (1/2)) class == SmallInteger ].
	FractionRun check: 'frac mul' that: [ (2/3) * (3/4) = (1/2) ].
	FractionRun check: 'frac div' equals: 2 is: [ (1/2) / (1/4) ].
	FractionRun check: 'reciprocal' that: [ (2/3) reciprocal = (3/2) ].
	FractionRun check: 'squared' that: [ (1/2) squared = (1/4) ].
]


[
	"mixed integer / fraction arithmetic (coercion)"
	FractionRun check: 'frac plus int' that: [ (1/2) + 1 = (3/2) ].
	FractionRun check: 'int plus frac' that: [ 1 + (1/2) = (3/2) ].
	FractionRun check: 'int times frac' equals: 1 is: [ 2 * (1/2) ].
	FractionRun check: 'frac times int' equals: 1 is: [ (1/2) * 2 ].
	FractionRun check: 'int minus frac' that: [ 3 - (1/2) = (5/2) ].
	FractionRun check: 'frac minus int' that: [ (1/2) - 3 = (-5/2) ].
	FractionRun check: 'int div int as frac' that: [ 1 / 3 = (1/3) ].
]


[
	"large fractions and comparing"
	FractionRun check: 'large denominator' that: [ (1 / (2 raisedTo: 70)) denominator = (2 raisedTo: 70) ].
	FractionRun check: 'large reduces to half' that: [ ((2 raisedTo: 70) / (2 raisedTo: 71)) = (1/2) ].
	FractionRun check: 'frac lt frac' that: [ (3/4) < (5/6) ].
	FractionRun check: 'frac lt int' that: [ (1/2) < 1 ].
	FractionRun check: 'int gt frac' that: [ 3 > (1/2) ].
	FractionRun check: 'equal after reduce' that: [ (1/2) = (2/4) ].
	FractionRun check: 'frac ne int' that: [ ((1/2) = 1) not ].
	FractionRun check: 'int ne frac' that: [ (1 = (1/2)) not ].
]


[
	"truncation, rounding and negative exponent"
	FractionRun check: 'truncated' equals: 3 is: [ (7/2) truncated ].
	FractionRun check: 'floor' equals: 3 is: [ (7/2) floor ].
	FractionRun check: 'neg floor' equals: -4 is: [ (-7/2) floor ].
	FractionRun check: 'neg truncated' equals: -3 is: [ (-7/2) truncated ].
	FractionRun check: 'ceiling' equals: 4 is: [ (7/2) ceiling ].
	FractionRun check: 'rounded up' equals: 1 is: [ (3/4) rounded ].
	FractionRun check: 'rounded down' equals: 0 is: [ (1/4) rounded ].
	FractionRun check: 'negative exponent' that: [ (2 raisedTo: -3) = (1/8) ].
]


[
	"errors for non-numbers"
	FractionRun expect: 'frac add non-number' raises: Error in: [(1/2) + Object].
	FractionRun expect: 'frac lt non-number' raises: Error in: [(1/2) < Object].
]

[ ^FractionRun report ]
