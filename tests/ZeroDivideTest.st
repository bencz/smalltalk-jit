"ZeroDivide: every zero-divisor path across the integer/fraction/large tower
 now raises a ZeroDivide (an ArithmeticError, and therefore an Error), not a
 bare `self error:`. The class is catchable at each level of the hierarchy and
 carries the standard 'division by zero' message text."

[ | t |
  t := TestRun named: 'ZeroDivide'.

  "SmallInteger paths raise ZeroDivide by its own class."
  t expect: 'SmallInteger / 0' raises: ZeroDivide in: [ 1 / 0 ].
  t expect: 'SmallInteger // 0' raises: ZeroDivide in: [ 1 // 0 ].
  t expect: 'SmallInteger \\ 0' raises: ZeroDivide in: [ 1 \\ 0 ].
  t expect: 'SmallInteger rem: 0' raises: ZeroDivide in: [ 1 rem: 0 ].
  t expect: 'SmallInteger quo: 0' raises: ZeroDivide in: [ 1 quo: 0 ].

  "Fraction: an explicit zero denominator and dividing a Fraction by zero."
  t expect: 'Fraction numerator:denominator: 0' raises: ZeroDivide in: [ Fraction numerator: 1 denominator: 0 ].
  t expect: 'Fraction / 0' raises: ZeroDivide in: [ (1 / 2) / 0 ].

  "LargeInteger truncating division by zero."
  t expect: 'LargeInteger // 0' raises: ZeroDivide in: [ (2 raisedTo: 100) // 0 ].
  t expect: 'LargeInteger rem: 0' raises: ZeroDivide in: [ (2 raisedTo: 100) rem: 0 ].
  t expect: 'LargeInteger / 0' raises: ZeroDivide in: [ (2 raisedTo: 100) / 0 ].

  "A ZeroDivide is also catchable as its superclass ArithmeticError and as the
   root Error: on:do: handler selection follows the class graph."
  t expect: 'catchable as ArithmeticError' raises: ArithmeticError in: [ 1 / 0 ].
  t expect: 'catchable as Error' raises: Error in: [ 1 / 0 ].

  "Class-graph facts and message text."
  t check: 'ZeroDivide is an ArithmeticError'
      that: [ ZeroDivide inheritsFrom: ArithmeticError ].
  t check: 'ArithmeticError is an Error'
      that: [ ArithmeticError inheritsFrom: Error ].
  t check: 'ZeroDivide is not resumable'
      that: [ ZeroDivide new isResumable not ].
  t check: 'messageText is division by zero'
      equals: 'division by zero'
      is: [ [ 1 / 0 ] on: ZeroDivide do: [ :e | e messageText ] ].
  t check: 'caught instance class is ZeroDivide'
      that: [ [ 5 \\ 0 ] on: ArithmeticError do: [ :e | e class == ZeroDivide ] ].

  ^t report
]
