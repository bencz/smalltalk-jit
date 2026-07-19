[
	"Division by zero must raise a catchable ZeroDivide, not SIGFPE-crash the VM.
	 Regression for the idiv primitives (IntQuo/IntMod/IntRem) that used to divide
	 without a zero check, faulting before the Smalltalk fallback could run. Since
	 this wave the fallback signals ZeroDivide (an ArithmeticError, hence an
	 Error) instead of a bare `self error:`, so the same sites are catchable at
	 every level of the exception hierarchy."
	DivideByZeroRun := TestRun named: 'DivideByZero'.

	DivideByZeroRun expect: 'idiv by zero' raises: ZeroDivide in: [5 // 0].
	DivideByZeroRun expect: 'mod by zero' raises: ZeroDivide in: [5 \\ 0].
	DivideByZeroRun expect: 'rem by zero' raises: ZeroDivide in: [5 rem: 0].
	DivideByZeroRun expect: 'quo by zero' raises: ZeroDivide in: [5 quo: 0].
	DivideByZeroRun expect: 'zero idiv zero' raises: ZeroDivide in: [0 // 0].
	DivideByZeroRun expect: 'neg mod zero' raises: ZeroDivide in: [-3 \\ 0].

	"A ZeroDivide stays catchable as its ArithmeticError superclass and as the
	 root Error, so existing on: Error do: handlers keep working."
	DivideByZeroRun expect: 'idiv by zero as ArithmeticError' raises: ArithmeticError in: [5 // 0].
	DivideByZeroRun expect: 'idiv by zero as Error' raises: Error in: [5 // 0].

	"Normal division/modulo must still be correct (floored) after the fix."
	DivideByZeroRun check: '17 idiv 5' equals: 3 is: [ 17 // 5 ].
	DivideByZeroRun check: '17 mod 5' equals: 2 is: [ 17 \\ 5 ].
	DivideByZeroRun check: '-17 idiv 5' equals: -4 is: [ -17 // 5 ].
	DivideByZeroRun check: '-17 mod 5' equals: 3 is: [ -17 \\ 5 ].
	DivideByZeroRun check: '17 idiv -5' equals: -4 is: [ 17 // -5 ].
	DivideByZeroRun check: '17 quo 5' equals: 3 is: [ 17 quo: 5 ].
	DivideByZeroRun check: '-17 quo 5' equals: -3 is: [ -17 quo: 5 ].
	DivideByZeroRun check: '17 rem 5' equals: 2 is: [ 17 rem: 5 ].
	DivideByZeroRun check: '-17 rem 5' equals: -2 is: [ -17 rem: 5 ].

	"SmallInteger>>asObject used to reach a printf+exit() stub that killed the VM;
	 it must now raise a catchable error instead."
	DivideByZeroRun expect: 'asObject raises' raises: Error in: [3 asObject].
]

[ ^DivideByZeroRun report ]
