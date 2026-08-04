"The emitted arithmetic fast path, checked where it actually runs.

 A send site computes the answer itself only AFTER the runtime has armed it
 (jit/InlineCache.h, IcInlineOp), and the runtime arms it on the first miss. So
 a one-shot check like `(3 + 4) = 7` never reaches the code this file is about:
 it measures the primitive, which was already correct. EVERY check here runs its
 operation in a LOOP, and the loop is what makes the site hot, armed and served
 by the emitted block from the second iteration on.

 What it is looking for is a wrong ANSWER, not a crash. The fast path re-derives
 SmallInteger overflow, SmallFloat64 encode and decode, IEEE comparison and the
 floor-versus-truncate difference in machine code, and every one of those is a
 place where it can quietly disagree with the primitive it replaced. The cases
 below are chosen to be exactly the boundaries where a plausible-looking
 sequence gives a plausible-looking wrong answer:

   * SmallInteger maxVal and minVal, where the 62-bit payload overflows and the
     answer has to become a LargeInteger through the Smalltalk fallback;
   * negative operands of // and \\, where the machine divide TRUNCATES and
     Smalltalk FLOORS, so the two disagree by one;
   * a zero divisor, which must raise and not answer;
   * negative zero, whose SmallFloat64 payload is 1 and not 0;
   * infinities and NaN, which the encoding refuses and every ordered
     comparison against NaN answers false for;
   * 1.0e300, whose product leaves the immediate window and has to box.

 A companion check that this file CANNOT make, because it needs two processes:
 the same program under ST_IC_STATS=1 must report identical send and way totals
 with ST_NO_INLINE_ARITH=1, which is what proves the fast path still feeds the
 optimizer's profile. run_tests.sh is where that pair belongs."

Hot := Object [
	"Run `block` enough times to arm the site inside it, answering how many of
	 the runs disagreed with `expected`. The count is what gets checked, so a
	 fast path that is right cold and wrong hot cannot pass by averaging."

	class count: n of: aBlock against: expected [
		| bad |
		bad := 0.
		1 to: n do: [ :i |
			aBlock value = expected ifFalse: [ bad := bad + 1 ] ].
		^bad
	]

	class countTrue: n of: aBlock [
		| bad |
		bad := 0.
		1 to: n do: [ :i |
			aBlock value ifFalse: [ bad := bad + 1 ] ].
		^bad
	]
]


[ IaRun := TestRun named: 'InlineArithmetic'.

  "integer arithmetic, hot"
  IaRun check: 'hot +' equals: 0 is: [ Hot count: 300 of: [ 17 + 25 ] against: 42 ].
  IaRun check: 'hot -' equals: 0 is: [ Hot count: 300 of: [ 17 - 25 ] against: 8 - 16 ].
  IaRun check: 'hot *' equals: 0 is: [ Hot count: 300 of: [ 17 * 25 ] against: 425 ].
  IaRun check: 'hot * by zero' equals: 0 is: [ Hot count: 300 of: [ 17 * 0 ] against: 0 ].
  IaRun check: 'hot * negative' equals: 0 is: [ Hot count: 300 of: [ 17 * (0 - 25) ] against: 0 - 425 ].

  "the 62-bit payload boundary: these must leave SmallInteger and the fast path
   must hand them over rather than wrap"
  IaRun check: 'maxVal + 1 is a LargeInteger' equals: 0 is: [
	Hot countTrue: 300 of: [ (SmallInteger maxVal + 1) isKindOf: LargeInteger ] ].
  IaRun check: 'maxVal + 1 value' equals: 0 is: [
	Hot count: 300 of: [ SmallInteger maxVal + 1 ] against: 2305843009213693952 ].
  IaRun check: 'minVal - 1 value' equals: 0 is: [
	Hot count: 300 of: [ SmallInteger minVal - 1 ] against: 0 - 2305843009213693953 ].
  IaRun check: 'maxVal * 2 value' equals: 0 is: [
	Hot count: 300 of: [ SmallInteger maxVal * 2 ] against: 4611686018427387902 ].
  IaRun check: 'maxVal + 0 stays small' equals: 0 is: [
	Hot countTrue: 300 of: [ (SmallInteger maxVal + 0) isKindOf: SmallInteger ] ].

  "bitwise, which is the one group where the tag survives untouched"
  IaRun check: 'hot bitAnd:' equals: 0 is: [ Hot count: 300 of: [ 12 bitAnd: 10 ] against: 8 ].
  IaRun check: 'hot bitOr:' equals: 0 is: [ Hot count: 300 of: [ 12 bitOr: 10 ] against: 14 ].
  IaRun check: 'hot bitXor:' equals: 0 is: [ Hot count: 300 of: [ 12 bitXor: 10 ] against: 6 ].
  IaRun check: 'bitAnd: with a negative' equals: 0 is: [
	Hot count: 300 of: [ (0 - 1) bitAnd: 255 ] against: 255 ].
  IaRun check: 'bitOr: with a negative' equals: 0 is: [
	Hot count: 300 of: [ (0 - 256) bitOr: 255 ] against: 0 - 1 ].
  IaRun check: 'bitXor: of equals is zero' equals: 0 is: [
	Hot count: 300 of: [ 1234 bitXor: 1234 ] against: 0 ].

  "// and \\ FLOOR. The machine divide truncates, so every negative operand here
   is a case where the fast path must refuse and let the kernel answer."
  IaRun check: 'hot // positive' equals: 0 is: [ Hot count: 300 of: [ 17 // 5 ] against: 3 ].
  IaRun check: 'hot \\ positive' equals: 0 is: [ Hot count: 300 of: [ 17 \\ 5 ] against: 2 ].
  IaRun check: 'exact //' equals: 0 is: [ Hot count: 300 of: [ 20 // 5 ] against: 4 ].
  IaRun check: 'exact \\ is zero' equals: 0 is: [ Hot count: 300 of: [ 20 \\ 5 ] against: 0 ].
  IaRun check: '// floors a negative dividend' equals: 0 is: [
	Hot count: 300 of: [ (0 - 17) // 5 ] against: 0 - 4 ].
  IaRun check: '\\ of a negative dividend is positive' equals: 0 is: [
	Hot count: 300 of: [ (0 - 17) \\ 5 ] against: 3 ].
  IaRun check: '// with a negative divisor' equals: 0 is: [
	Hot count: 300 of: [ 17 // (0 - 5) ] against: 0 - 4 ].
  IaRun check: '\\ with a negative divisor' equals: 0 is: [
	Hot count: 300 of: [ 17 \\ (0 - 5) ] against: 0 - 3 ].
  IaRun check: '// by zero raises' that: [
	[ Hot count: 300 of: [ 17 // 0 ] against: 0. false ]
		on: ZeroDivide do: [ :e | true ] ].
  IaRun check: '\\ by zero raises' that: [
	[ Hot count: 300 of: [ 17 \\ 0 ] against: 0. false ]
		on: ZeroDivide do: [ :e | true ] ].

  "the relational six on integers"
  IaRun check: 'hot <' equals: 0 is: [ Hot countTrue: 300 of: [ 3 < 4 ] ].
  IaRun check: 'hot < false side' equals: 0 is: [ Hot countTrue: 300 of: [ (4 < 3) not ] ].
  IaRun check: 'hot >' equals: 0 is: [ Hot countTrue: 300 of: [ 4 > 3 ] ].
  IaRun check: 'hot <=' equals: 0 is: [ Hot countTrue: 300 of: [ 3 <= 3 ] ].
  IaRun check: 'hot >=' equals: 0 is: [ Hot countTrue: 300 of: [ 3 >= 3 ] ].
  IaRun check: 'hot =' equals: 0 is: [ Hot countTrue: 300 of: [ 3 = 3 ] ].
  IaRun check: 'hot ~=' equals: 0 is: [ Hot countTrue: 300 of: [ 3 ~= 4 ] ].
  IaRun check: 'comparison across the sign' equals: 0 is: [
	Hot countTrue: 300 of: [ (0 - 5) < 5 ] ].
  IaRun check: 'minVal is below maxVal' equals: 0 is: [
	Hot countTrue: 300 of: [ SmallInteger minVal < SmallInteger maxVal ] ].
]


[ "floats: the encode and decode the fast path re-derives in machine code"
  IaRun check: 'hot float +' equals: 0 is: [ Hot count: 300 of: [ 1.5 + 2.25 ] against: 3.75 ].
  IaRun check: 'hot float -' equals: 0 is: [ Hot count: 300 of: [ 1.5 - 2.25 ] against: 0.0 - 0.75 ].
  IaRun check: 'hot float *' equals: 0 is: [ Hot count: 300 of: [ 1.5 * 2.25 ] against: 3.375 ].
  IaRun check: 'hot float /' equals: 0 is: [ Hot count: 300 of: [ 1.5 / 2.0 ] against: 0.75 ].

  "zero, and the negative zero whose payload is 1 rather than 0"
  IaRun check: 'float zero sum' equals: 0 is: [ Hot count: 300 of: [ 0.0 + 0.0 ] against: 0.0 ].
  IaRun check: 'negative zero equals zero' equals: 0 is: [
	Hot countTrue: 300 of: [ (0.0 - 0.0) = 0.0 ] ].
  IaRun check: 'zero times a value' equals: 0 is: [ Hot count: 300 of: [ 0.0 * 1.5 ] against: 0.0 ].
  IaRun check: 'a value minus itself is zero' equals: 0 is: [
	Hot count: 300 of: [ 3.25 - 3.25 ] against: 0.0 ].

  "results the immediate encoding cannot hold, so the fast path must hand over"
  IaRun check: 'division by zero is infinite' equals: 0 is: [
	Hot countTrue: 300 of: [ (1.0 / 0.0) isInfinite ] ].
  IaRun check: 'zero over zero is NaN' equals: 0 is: [
	Hot countTrue: 300 of: [ (0.0 / 0.0) isNaN ] ].
  IaRun check: 'overflow to infinity' equals: 0 is: [
	Hot countTrue: 300 of: [ (1.0e300 * 1.0e300) isInfinite ] ].
  IaRun check: 'a large product survives' equals: 0 is: [
	Hot count: 300 of: [ 1.0e300 + 1.0e300 ] against: 2.0e300 ].
  IaRun check: 'a denormal survives' equals: 0 is: [
	Hot count: 300 of: [ 2.2250738585072014e-308 * 1.0 ] against: 2.2250738585072014e-308 ].

  "the rounding the encoding must not disturb"
  IaRun check: 'tenths do not become exact' equals: 0 is: [
	Hot count: 300 of: [ 0.1 + 0.2 ] against: 0.30000000000000004 ].
  IaRun check: 'a tenth is not one over ten' equals: 0 is: [
	Hot countTrue: 300 of: [ (0.1 asExactFraction = (1/10)) not ] ].

  "the relational six on floats"
  IaRun check: 'hot float <' equals: 0 is: [ Hot countTrue: 300 of: [ 1.5 < 2.0 ] ].
  IaRun check: 'hot float >' equals: 0 is: [ Hot countTrue: 300 of: [ 2.0 > 1.5 ] ].
  IaRun check: 'hot float <=' equals: 0 is: [ Hot countTrue: 300 of: [ 1.5 <= 1.5 ] ].
  IaRun check: 'hot float >=' equals: 0 is: [ Hot countTrue: 300 of: [ 1.5 >= 1.5 ] ].
  IaRun check: 'hot float =' equals: 0 is: [ Hot countTrue: 300 of: [ 1.5 = 1.5 ] ].
  IaRun check: 'hot float ~=' equals: 0 is: [ Hot countTrue: 300 of: [ 1.5 ~= 2.5 ] ].

  "EVERY ordered comparison against NaN is false, and ~= against it is true.
   This is the one place where the emitted sequence cannot use the carry flag
   alone: an SSE compare reports NaN in the parity flag and sets the others to
   the same pattern `below` produces."
  IaRun check: 'NaN is not less' equals: 0 is: [
	Hot countTrue: 300 of: [ ((0.0 / 0.0) < 1.0) not ] ].
  IaRun check: 'NaN is not greater' equals: 0 is: [
	Hot countTrue: 300 of: [ ((0.0 / 0.0) > 1.0) not ] ].
  IaRun check: 'NaN is not less or equal' equals: 0 is: [
	Hot countTrue: 300 of: [ ((0.0 / 0.0) <= 1.0) not ] ].
  IaRun check: 'NaN is not greater or equal' equals: 0 is: [
	Hot countTrue: 300 of: [ ((0.0 / 0.0) >= 1.0) not ] ].
  IaRun check: 'NaN does not equal itself' equals: 0 is: [
	Hot countTrue: 300 of: [ ((0.0 / 0.0) = (0.0 / 0.0)) not ] ].
  IaRun check: 'NaN differs from itself' equals: 0 is: [
	Hot countTrue: 300 of: [ (0.0 / 0.0) ~= (0.0 / 0.0) ] ].
  IaRun check: 'infinity compares' equals: 0 is: [
	Hot countTrue: 300 of: [ 1.0 < (1.0 / 0.0) ] ].
]


[ "mixed pairs, which the fast path must REFUSE: its tag test proves both
   operands share a representation, and these do not. The answers have to be the
   coercing ones the numeric tower gives."
  IaRun check: 'int plus float' equals: 0 is: [ Hot count: 300 of: [ 3 + 0.5 ] against: 3.5 ].
  IaRun check: 'float plus int' equals: 0 is: [ Hot count: 300 of: [ 0.5 + 3 ] against: 3.5 ].
  IaRun check: 'int times float' equals: 0 is: [ Hot count: 300 of: [ 3 * 0.5 ] against: 1.5 ].
  IaRun check: 'int below float' equals: 0 is: [ Hot countTrue: 300 of: [ 3 < 3.5 ] ].
  IaRun check: 'float above int' equals: 0 is: [ Hot countTrue: 300 of: [ 3.5 > 3 ] ].
  IaRun check: 'int equals its float' equals: 0 is: [ Hot countTrue: 300 of: [ 3 = 3.0 ] ].

  "and pairs where one side is not an immediate at all, so the tag test fails
   and the ordinary send answers"
  IaRun check: 'int below a LargeInteger' equals: 0 is: [
	Hot countTrue: 300 of: [ 3 < (2 raisedTo: 100) ] ].
  IaRun check: 'a LargeInteger is above an int' equals: 0 is: [
	Hot countTrue: 300 of: [ (2 raisedTo: 100) > 3 ] ].
  IaRun check: 'int does not equal a LargeInteger' equals: 0 is: [
	Hot countTrue: 300 of: [ (3 = (2 raisedTo: 100)) not ] ].
  IaRun check: 'int plus a Fraction' equals: 0 is: [
	Hot count: 300 of: [ 3 + (1/2) ] against: (7/2) ].
  IaRun check: 'int above a Fraction' equals: 0 is: [
	Hot countTrue: 300 of: [ 3 > (1/2) ] ].
  IaRun check: 'an int does not equal nil' equals: 0 is: [
	Hot countTrue: 300 of: [ (3 = nil) not ] ].
  IaRun check: 'an int differs from nil' equals: 0 is: [
	Hot countTrue: 300 of: [ 3 ~= nil ] ].

  "THE CARET IS THE GATE. TestRun.st says so in its own header: the fail count
   this answers becomes the process exit code, and run_tests.sh gates on that.
   Written without it, every check in this file could go red and the file would
   still exit 0 -- verified by injection while writing it."
  ^IaRun report
]
