"=== Standard test harness. Paste verbatim at the top of each tests/*.st file.
 Every check is wrapped so an unexpected runtime error counts as a FAILURE
 (a raw Assert failure or uncaught error prints a backtrace yet still exits 0,
 i.e. a false pass). The final top-level block MUST end with ^t report, whose
 integer return becomes the process exit code (0 = pass) that run_tests.sh
 gates on. Do not use the old Assert class in new tests. All catches use
 Exception (the root) so a wrong error-class name surfaces as a visible FAIL,
 never a silent exit-0. ==="
TestRun := Object [
    | fails count name |
    class named: aString [ ^self new setName: aString ]
    setName: aString [ name := aString. fails := 0. count := 0. ^self ]
    fail: aLabel [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' FAIL: '; nextPutAll: aLabel; lf.
    ]
    error: aLabel from: e [
        fails := fails + 1.
        Transcript nextPutAll: name; nextPutAll: ' ERROR ['; nextPutAll: aLabel;
            nextPutAll: ']: '; nextPutAll: e messageText; lf.
    ]
    check: aLabel that: aBlock [
        count := count + 1.
        [ (aBlock value = true) ifFalse: [ self fail: aLabel ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    check: aLabel equals: expected is: aBlock [
        count := count + 1.
        [ | got |
          got := aBlock value.
          got = expected ifFalse: [
              self fail: aLabel, ' (got ', got printString, ' expected ', expected printString, ')' ] ]
            on: Exception do: [:e | self error: aLabel from: e ].
    ]
    expect: aLabel raises: aClass in: aBlock [
        count := count + 1.
        [ aBlock value. self fail: aLabel, ' (no error raised)' ]
            on: Exception do: [:e |
                (aClass notNil and: [ e isKindOf: aClass ]) ifFalse: [
                    self fail: aLabel, ' (raised ', e class printString, ' not ', aClass printString, ')' ] ].
    ]
    report [
        Transcript nextPutAll: name; nextPutAll: ': ';
            nextPutAll: (count - fails) printString; nextPutAll: '/';
            nextPutAll: count printString; nextPutAll: ' ok'; lf.
        ^fails
    ]
]

"Number coercion (generality-based, Blue Book style):
 SmallInteger 100 < LargeInteger 300 < Fraction 400 < Float 500.
 Mixed arithmetic coerces up to the higher generality, exact results demote
 back down (a Fraction whose denominator reduces to 1 becomes an Integer).
 We also cover the conversion protocol and rounding/truncation modes.
 Float results are compared with a tolerance, never exact =."

[ CoerceRun := TestRun named: 'NumberCoercion'.
  "result type follows the higher generality operand"
  CoerceRun check: 'int + float is a Float' that: [ (1 + 2.5) isKindOf: Float ].
  CoerceRun check: 'float + int is a Float' that: [ (2.5 + 1) isKindOf: Float ].
  CoerceRun check: 'int + fraction is a Fraction' that: [ (2 + (1/2)) class == Fraction ].
  CoerceRun check: 'fraction + int is a Fraction' that: [ ((1/2) + 2) class == Fraction ].
  CoerceRun check: 'float + fraction is a Float' that: [ (0.5 + (1/2)) isKindOf: Float ].
  CoerceRun check: 'fraction + float is a Float' that: [ ((1/2) + 0.5) isKindOf: Float ].
  "exact fraction result demotes to Integer"
  CoerceRun check: 'fraction + fraction demotes to SmallInteger' that: [ ((1/2) + (1/2)) class == SmallInteger ].
  CoerceRun check: 'fraction + fraction value' equals: 1 is: [ (1/2) + (1/2) ].
  CoerceRun check: 'int * fraction demotes' equals: 1 is: [ 2 * (1/2) ].
  CoerceRun check: 'int * fraction demotes class' that: [ (2 * (1/2)) class == SmallInteger ].
  CoerceRun check: 'int + fraction value' equals: (5/2) is: [ 2 + (1/2) ].
  CoerceRun check: 'int - fraction value' equals: (5/2) is: [ 3 - (1/2) ].
  "coercion is symmetric in value and in result class"
  CoerceRun check: 'add symmetric value' that: [ (1 + 2.5) = (2.5 + 1) ].
  CoerceRun check: 'mul symmetric value' that: [ (2 * (1/2)) = ((1/2) * 2) ].
  CoerceRun check: 'add symmetric class' that: [ (1 + 2.5) class == (2.5 + 1) class ].
]

[ "mixed-type comparison and equality coerce to a common generality"
  CoerceRun check: '1 = 1.0' that: [ 1 = 1.0 ].
  CoerceRun check: '1.0 = 1' that: [ 1.0 = 1 ].
  CoerceRun check: '(1/2) = 0.5' that: [ (1/2) = 0.5 ].
  CoerceRun check: '0.5 = (1/2)' that: [ 0.5 = (1/2) ].
  CoerceRun check: '(3/2) = 1.5' that: [ (3/2) = 1.5 ].
  CoerceRun check: '(1/2) < 0.6' that: [ (1/2) < 0.6 ].
  CoerceRun check: 'not 0.6 < (1/2)' that: [ (0.6 < (1/2)) not ].
  CoerceRun check: '2 < 2.5' that: [ 2 < 2.5 ].
  CoerceRun check: '2.5 < 3' that: [ 2.5 < 3 ].
  CoerceRun check: 'not 2.5 < (5/2) (equal)' that: [ (2.5 < (5/2)) not ].
  CoerceRun check: 'not 2.5 = 3' that: [ (2.5 = 3) not ].
  CoerceRun check: '1 ~= 1.5' that: [ 1 ~= 1.5 ].
  "value equality across representations implies equal hashes"
  CoerceRun check: 'hash 1 = hash 1.0' that: [ 1 hash = 1.0 hash ].
  CoerceRun check: 'hash 2 = hash 2.0' that: [ 2 hash = 2.0 hash ].
]

[ "conversion protocol: asFloat / asInteger / asFraction and truncation"
  CoerceRun check: '(1/2) asFloat' that: [ ((1/2) asFloat - 0.5) abs < 1e-9 ].
  CoerceRun check: '3 asFloat' that: [ (3 asFloat - 3.0) abs < 1e-9 ].
  CoerceRun check: '3 asFloat is a Float' that: [ 3 asFloat isKindOf: Float ].
  CoerceRun check: '3.7 asInteger truncates' equals: 3 is: [ 3.7 asInteger ].
  CoerceRun check: 'neg3.7 asInteger truncates toward 0' equals: 3 negated is: [ 3.7 negated asInteger ].
  CoerceRun check: '5 asInteger is self' equals: 5 is: [ 5 asInteger ].
  CoerceRun check: 'fraction asFraction is self' that: [ | f | f := (3/4). f asFraction == f ].
  "truncated / floor / ceiling / rounded across Float and Fraction"
  CoerceRun check: '3.7 truncated' equals: 3 is: [ 3.7 truncated ].
  CoerceRun check: '(7/2) truncated' equals: 3 is: [ (7/2) truncated ].
  CoerceRun check: 'neg(7/2) truncated toward 0' equals: 3 negated is: [ (7/2) negated truncated ].
  CoerceRun check: '3.7 floor' equals: 3 is: [ 3.7 floor ].
  CoerceRun check: 'neg(7/2) floor toward -inf' equals: 4 negated is: [ (7/2) negated floor ].
  CoerceRun check: '3.2 ceiling' equals: 4 is: [ 3.2 ceiling ].
  CoerceRun check: '(7/2) ceiling' equals: 4 is: [ (7/2) ceiling ].
  CoerceRun check: 'neg(7/2) ceiling toward +inf' equals: 3 negated is: [ (7/2) negated ceiling ].
  CoerceRun check: '3.5 rounded' equals: 4 is: [ 3.5 rounded ].
  CoerceRun check: '2.4 rounded' equals: 2 is: [ 2.4 rounded ].
  CoerceRun check: '2.5 rounded (half away from zero)' equals: 3 is: [ 2.5 rounded ].
  CoerceRun check: 'neg2.5 rounded (half away from zero)' equals: 3 negated is: [ 2.5 negated rounded ].
  CoerceRun check: '(3/4) rounded' equals: 1 is: [ (3/4) rounded ].
  CoerceRun check: '(1/4) rounded' equals: 0 is: [ (1/4) rounded ].
  CoerceRun check: 'neg(3/2) rounded (fraction rounds toward +inf)' equals: 1 negated is: [ (3/2) negated rounded ].
]

[ "roundTo: / truncateTo: / reciprocal / squared / sqrt (float via tolerance)"
  CoerceRun check: '17 roundTo 5' equals: 15 is: [ 17 roundTo: 5 ].
  CoerceRun check: '13 roundTo 5' equals: 15 is: [ 13 roundTo: 5 ].
  CoerceRun check: '2 roundTo 5' equals: 0 is: [ 2 roundTo: 5 ].
  CoerceRun check: '3.7 roundTo 0.5' that: [ ((3.7 roundTo: 0.5) - 3.5) abs < 1e-9 ].
  CoerceRun check: '17 truncateTo 5' equals: 15 is: [ 17 truncateTo: 5 ].
  CoerceRun check: '19 truncateTo 5' equals: 15 is: [ 19 truncateTo: 5 ].
  CoerceRun check: '3.7 truncateTo 0.5' that: [ ((3.7 truncateTo: 0.5) - 3.5) abs < 1e-9 ].
  CoerceRun check: '2 reciprocal is a Fraction' equals: (1/2) is: [ 2 reciprocal ].
  CoerceRun check: '(2/3) reciprocal' equals: (3/2) is: [ (2/3) reciprocal ].
  CoerceRun check: '0.5 reciprocal' that: [ (0.5 reciprocal - 2.0) abs < 1e-9 ].
  CoerceRun check: '3 squared' equals: 9 is: [ 3 squared ].
  CoerceRun check: '(2/3) squared' equals: (4/9) is: [ (2/3) squared ].
  CoerceRun check: '1.5 squared' that: [ (1.5 squared - 2.25) abs < 1e-9 ].
  CoerceRun check: '4 sqrt' that: [ (4 sqrt - 2.0) abs < 1e-9 ].
  CoerceRun check: '2 sqrt' that: [ (2 sqrt - 1.4142135623730951) abs < 1e-9 ].
  CoerceRun check: '(1/4) sqrt' that: [ ((1/4) sqrt - 0.5) abs < 1e-9 ].
  CoerceRun check: 'int sqrt equals float sqrt' that: [ 2 sqrt = 2.0 sqrt ].
  ^CoerceRun report
]
