[
	"Division by zero must raise a catchable Error, not SIGFPE-crash the VM.
	 Regression for the idiv primitives (IntQuo/IntMod/IntRem) that used to divide
	 without a zero check, faulting before the Smalltalk fallback could run."
	Assert do: [5 // 0] expect: Error.
	Assert do: [5 \\ 0] expect: Error.
	Assert do: [5 rem: 0] expect: Error.
	Assert do: [5 quo: 0] expect: Error.
	Assert do: [0 // 0] expect: Error.
	Assert do: [-3 \\ 0] expect: Error.

	"Normal division/modulo must still be correct (floored) after the fix."
	Assert true: (17 // 5) = 3.
	Assert true: (17 \\ 5) = 2.
	Assert true: (-17 // 5) = -4.
	Assert true: (-17 \\ 5) = 3.
	Assert true: (17 // -5) = -4.
	Assert true: (17 quo: 5) = 3.
	Assert true: (-17 quo: 5) = -3.
	Assert true: (17 rem: 5) = 2.
	Assert true: (-17 rem: 5) = -2.

	"SmallInteger>>asObject used to reach a printf+exit() stub that killed the VM;
	 it must now raise a catchable error instead."
	Assert do: [3 asObject] expect: Error.
]
