"Integer protocol: divisibility, parity, bit manipulation, radix printing,
 the four division operators (floored // \\ vs truncating quo: rem:),
 exponentiation with LargeInteger promotion, and the iteration protocol.
 Split across blocks to stay well under the compiler large-method limit."

[ IntRun := TestRun named: 'IntegerProtocol'.
  "gcd: / lcm: (both take abs; gcd of 0 is the other operand)"
  IntRun check: 'gcd 12 18' equals: 6 is: [ 12 gcd: 18 ].
  IntRun check: 'gcd 0 5' equals: 5 is: [ 0 gcd: 5 ].
  IntRun check: 'gcd coprime 17 5' equals: 1 is: [ 17 gcd: 5 ].
  IntRun check: 'gcd of negatives (abs)' equals: 4 is: [ | a | a := 12 negated. a gcd: 8 ].
  IntRun check: 'lcm 12 18' equals: 36 is: [ 12 lcm: 18 ].
  IntRun check: 'lcm 4 6' equals: 12 is: [ 4 lcm: 6 ].
  IntRun check: 'lcm with 0 is 0' equals: 0 is: [ 0 lcm: 5 ].
  "factorial (0! = 1, promotes to LargeInteger, negative is an error)"
  IntRun check: '0 factorial' equals: 1 is: [ 0 factorial ].
  IntRun check: '1 factorial' equals: 1 is: [ 1 factorial ].
  IntRun check: '5 factorial' equals: 120 is: [ 5 factorial ].
  IntRun check: '6 factorial' equals: 720 is: [ 6 factorial ].
  IntRun expect: 'factorial of negative raises' raises: Error in: [ | n | n := 3 negated. n factorial ].
  "even / odd, including negatives (two-complement low bit)"
  IntRun check: '4 even' that: [ 4 even ].
  IntRun check: 'not 4 odd' that: [ 4 odd not ].
  IntRun check: '7 odd' that: [ 7 odd ].
  IntRun check: '0 even' that: [ 0 even ].
  IntRun check: 'negative 3 is odd' that: [ | n | n := 3 negated. n odd ].
  IntRun check: 'negative 4 is even' that: [ | n | n := 4 negated. n even ].
  "abs / negated / sign at and around zero"
  IntRun check: '5 abs' equals: 5 is: [ 5 abs ].
  IntRun check: 'negated 5 abs' equals: 5 is: [ 5 negated abs ].
  IntRun check: '0 abs' equals: 0 is: [ 0 abs ].
  IntRun check: '5 negated' equals: 5 negated is: [ 0 - 5 ].
  IntRun check: 'double negated' equals: 5 is: [ 5 negated negated ].
  IntRun check: '5 sign' equals: 1 is: [ 5 sign ].
  IntRun check: 'negative sign' equals: 1 negated is: [ 5 negated sign ].
  IntRun check: '0 sign' equals: 0 is: [ 0 sign ].
]

[ "bit manipulation and radix printing"
  IntRun check: '12 bitAnd 10' equals: 8 is: [ 12 bitAnd: 10 ].
  IntRun check: '12 bitOr 10' equals: 14 is: [ 12 bitOr: 10 ].
  IntRun check: '12 bitXor 10' equals: 6 is: [ 12 bitXor: 10 ].
  IntRun check: 'bitShift left 1 by 4' equals: 16 is: [ 1 bitShift: 4 ].
  IntRun check: 'bitShift left 3 by 8' equals: 768 is: [ 3 bitShift: 8 ].
  IntRun check: 'bitShift right 16 by -2' equals: 4 is: [ 16 bitShift: 2 negated ].
  IntRun check: 'bitShift right 255 by -1' equals: 127 is: [ 255 bitShift: 1 negated ].
  IntRun check: '5 bitInvert' equals: 6 negated is: [ 5 bitInvert ].
  IntRun check: '0 bitInvert' equals: 1 negated is: [ 0 bitInvert ].
  IntRun check: 'highBit 1' equals: 1 is: [ 1 highBit ].
  IntRun check: 'highBit 255' equals: 8 is: [ 255 highBit ].
  IntRun check: 'highBit 256' equals: 9 is: [ 256 highBit ].
  IntRun check: 'highBit 0' equals: 0 is: [ 0 highBit ].
  IntRun check: 'printStringBase 16 of 255' equals: 'FF' is: [ 255 printStringBase: 16 ].
  IntRun check: 'printStringBase 2 of 10' equals: '1010' is: [ 10 printStringBase: 2 ].
  IntRun check: 'printStringBase 8 of 8' equals: '10' is: [ 8 printStringBase: 8 ].
  IntRun check: 'printStringBase 10 of 255' equals: '255' is: [ 255 printStringBase: 10 ].
  IntRun check: 'printStringBase 16 of negative' equals: '-FF' is: [ 255 negated printStringBase: 16 ].
]

[ "floored division // \\ vs truncating quo: rem: (sign behaviour differs)"
  IntRun check: '17 // 5' equals: 3 is: [ 17 // 5 ].
  IntRun check: 'neg17 // 5 (floors toward -inf)' equals: 4 negated is: [ 17 negated // 5 ].
  IntRun check: '17 // neg5' equals: 4 negated is: [ 17 // 5 negated ].
  IntRun check: 'neg17 // neg5' equals: 3 is: [ 17 negated // 5 negated ].
  IntRun check: '17 \\ 5' equals: 2 is: [ 17 \\ 5 ].
  IntRun check: 'neg17 \\ 5 (sign of divisor)' equals: 3 is: [ 17 negated \\ 5 ].
  IntRun check: '17 \\ neg5' equals: 3 negated is: [ 17 \\ 5 negated ].
  IntRun check: 'neg17 \\ neg5' equals: 2 negated is: [ 17 negated \\ 5 negated ].
  IntRun check: '17 quo 5' equals: 3 is: [ 17 quo: 5 ].
  IntRun check: 'neg17 quo 5 (truncates toward 0)' equals: 3 negated is: [ 17 negated quo: 5 ].
  IntRun check: '17 rem 5' equals: 2 is: [ 17 rem: 5 ].
  IntRun check: 'neg17 rem 5 (sign of dividend)' equals: 2 negated is: [ 17 negated rem: 5 ].
  IntRun check: 'floored identity q*b + r = a' that: [ ((17 // 5) * 5) + (17 \\ 5) = 17 ].
  IntRun check: 'floored identity negative' that: [ ((17 negated // 5) * 5) + (17 negated \\ 5) = 17 negated ].
  IntRun check: 'truncating identity q*b + r = a' that: [ ((17 negated quo: 5) * 5) + (17 negated rem: 5) = 17 negated ].
  "division by zero raises a plain Error (no ZeroDivide in this VM)"
  IntRun expect: '5 // 0 raises' raises: Error in: [ 5 // 0 ].
  IntRun expect: '5 \\ 0 raises' raises: Error in: [ 5 \\ 0 ].
  IntRun expect: '5 quo 0 raises' raises: Error in: [ 5 quo: 0 ].
  IntRun expect: '5 rem 0 raises' raises: Error in: [ 5 rem: 0 ].
  IntRun expect: '5 / 0 raises' raises: Error in: [ 5 / 0 ].
]

[ "exponentiation, LargeInteger promotion, and the iteration protocol"
  IntRun check: '2 raisedTo 10' equals: 1024 is: [ 2 raisedTo: 10 ].
  IntRun check: '2 raisedTo 0' equals: 1 is: [ 2 raisedTo: 0 ].
  IntRun check: '10 raisedTo 3' equals: 1000 is: [ 10 raisedTo: 3 ].
  IntRun check: '2 raisedTo -3 is a Fraction' equals: (1/8) is: [ 2 raisedTo: 3 negated ].
  IntRun check: '5 raisedToInteger 2' equals: 25 is: [ 5 raisedToInteger: 2 ].
  IntRun check: '2 raisedTo 100 (large, exact)' equals: 1267650600228229401496703205376 is: [ 2 raisedTo: 100 ].
  IntRun check: '(2 raisedTo 10) = 1024' that: [ (2 raisedTo: 10) = 1024 ].
  IntRun check: '2 raisedTo 100 is a LargeInteger' that: [ (2 raisedTo: 100) isKindOf: LargeInteger ].
  IntRun check: '20 factorial (large, exact)' equals: 2432902008176640000 is: [ 20 factorial ].
  IntRun check: '100 factorial printString size' equals: 158 is: [ 100 factorial printString size ].
  IntRun check: 'timesRepeat accumulates' equals: 5 is: [ | s | s := 0. 5 timesRepeat: [ s := s + 1 ]. s ].
  IntRun check: '0 timesRepeat is a no-op' equals: 0 is: [ | s | s := 0. 0 timesRepeat: [ s := s + 1 ]. s ].
  IntRun check: 'to:do: sums 1..5' equals: 15 is: [ | s | s := 0. 1 to: 5 do: [ :i | s := s + i ]. s ].
  IntRun check: 'to:by:do: negative step sum' equals: 55 is: [ | s | s := 0. 10 to: 1 by: 1 negated do: [ :i | s := s + i ]. s ].
  IntRun check: 'to:by:do: negative step count' equals: 10 is: [ | c | c := 0. 10 to: 1 by: 1 negated do: [ :i | c := c + 1 ]. c ].
  IntRun check: 'to:by:do: step 2 sum' equals: 25 is: [ | s | s := 0. 1 to: 10 by: 2 do: [ :i | s := s + i ]. s ].
  IntRun check: 'to:by:do: empty when step wrong way' equals: 0 is: [ | c | c := 0. 1 to: 10 by: 1 negated do: [ :i | c := c + 1 ]. c ].
  IntRun check: '5 max 3' equals: 5 is: [ 5 max: 3 ].
  IntRun check: '5 min 3' equals: 3 is: [ 5 min: 3 ].
  ^IntRun report
]
