"Regression: variables the JIT SPILLED must still be readable and writable.

 A spilled variable's `reg` is SPILLED_REG (-1). Handing that -1 to an emitter as a
 register NUMBER encodes a wild register: on x64 it is R15 in a ModRM reg field (and
 R15 is IN the allocatable pool, so it silently clobbers a live variable), and it is
 RIP in a memory base, so the access runs against the instruction stream. POWER has
 never had this (varReg() materializes spilled variables in r4); x64 grew the same
 discipline as fillVarToReg()/fillContextToReg().

 The two shapes covered here, both of which used to corrupt silently:

   1. OUTER CONTEXT (fillContext). Only the level-0 context is pinned to a register,
      so an outer context can be spilled. Any block reading an enclosing method's
      variable goes through this. It was doubly bad: it also marked the spilled
      variable VAR_IN_REG, so every later use read -1 as an address base and nothing
      was emitted to correct it. That made it invisible even to an ASSERT.

   2. GLOBAL STORE (movToOperand/OPERAND_ASSOC). The read twin was already guarded;
      the write twin was not. A spilled assoc has no frame slot either, so it must be
      rematerialized from the literal frame.

 These only spill once a method outgrows the register pool, so run this under
 ST_JIT_REGS to force it:

     for n in 3 4 5 6 7 8 9; do ST_JIT_REGS=$n ./run_tests.sh --all; done

 At the default pool this file still passes, but it does NOT prove much on its own:
 the pressure sweep is what actually exercises the spill paths."

SpillCtx := Object [

	"A block reading MANY enclosing variables. Every read is an OPERAND_CONTEXT_VAR at
	 level >= 1. The count matters: each send receiver also burns a shadow VAR_CLASS
	 and each global a VAR_ASSOC out of the SAME pool, so real demand is well above the
	 source temp count, and this is what drives the context itself out to a spill."
	outerReads [
		| a b c d e f g h i j k l blk |
		a := 1. b := 2. c := 3. d := 4.
		e := 5. f := 6. g := 7. h := 8.
		i := 9. j := 10. k := 11. l := 12.
		blk := [:x | x + a + b + c + d + e + f + g + h + i + j + k + l].
		^blk value: 0
	]


	"Same, but the block WRITES the enclosing variables too (store path through the
	 outer context, which is where generateStoreCheck also gets involved)."
	outerWrites [
		| a b c d e f g h i j k l blk |
		a := 0. b := 0. c := 0. d := 0.
		e := 0. f := 0. g := 0. h := 0.
		i := 0. j := 0. k := 0. l := 0.
		blk := [:x |
			a := x. b := x + 1. c := x + 2. d := x + 3.
			e := x + 4. f := x + 5. g := x + 6. h := x + 7.
			i := x + 8. j := x + 9. k := x + 10. l := x + 11.
			a + b + c + d + e + f + g + h + i + j + k + l].
		^blk value: 1
	]


	"Nested blocks: the inner one reads level-2 context."
	nestedOuterReads [
		| a b c d outer |
		a := 10. b := 20. c := 30. d := 40.
		outer := [:x | | inner | inner := [:y | y + a + b + c + d]. inner value: x].
		^outer value: 5
	]
]

"Globals: the store path (movToOperand/OPERAND_ASSOC) is the one that was unguarded.
 Several distinct globals in one method, because each burns a pool slot of its own."
[
	SpillG1 := 0. SpillG2 := 0. SpillG3 := 0. SpillG4 := 0.
]

SpillGlobals := Object [

	"Writes several globals with many locals live across them, so the assoc variables
	 compete for the pool and get spilled."
	writeGlobals [
		| a b c d e f g h |
		a := 1. b := 2. c := 3. d := 4.
		e := 5. f := 6. g := 7. h := 8.
		SpillG1 := a + b.
		SpillG2 := c + d.
		SpillG3 := e + f.
		SpillG4 := g + h.
		^SpillG1 + SpillG2 + SpillG3 + SpillG4 + a + b + c + d + e + f + g + h
	]


	"Stores a POINTER (not a tagged immediate) into a global, so the write barrier
	 (generateStoreCheck) runs with the assoc as its object operand."
	writeGlobalPointer [
		| a b c d e |
		a := OrderedCollection new.
		b := 1. c := 2. d := 3. e := 4.
		a add: b + c + d + e.
		SpillG1 := a.
		^(SpillG1 first) + b + c + d + e
	]
]

[
	| fails check |
	fails := 0.

	"An uncaught error kills the fiber while main.c still exits 0, which
	 run_tests.sh reads as a pass, so catch it and count it instead."
	check := [:name :expected :block |
		[block value = expected
			ifFalse: [
				fails := fails + 1.
				Transcript nextPutAll: 'FAIL (wrong value): '; nextPutAll: name; lf]]
			on: Error
			do: [:e |
				fails := fails + 1.
				Transcript nextPutAll: 'FAIL (raised): '; nextPutAll: name; lf]].

	check value: 'outerReads' value: 78 value: [SpillCtx new outerReads].
	check value: 'outerWrites' value: 78 value: [SpillCtx new outerWrites].
	check value: 'nestedOuterReads' value: 105 value: [SpillCtx new nestedOuterReads].
	check value: 'writeGlobals' value: 72 value: [SpillGlobals new writeGlobals].
	check value: 'writeGlobalPointer' value: 20 value: [SpillGlobals new writeGlobalPointer].

	fails = 0 ifTrue: [Transcript nextPutAll: 'SpilledVarTest: OK'; lf].
	^fails
]
