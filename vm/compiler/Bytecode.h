#ifndef BYTECODE_H
#define BYTECODE_H

// The register bytecode (docs/jit-v2/04-bytecode.md).
//
// Flat virtual registers, three-address, FIXED 8-byte instructions. Each of
// those three properties is there to serve a specific consumer:
//
//   flat registers   a register becomes an SSA value by definition, with no
//                    operand to interpret first. The old bytecode's operands
//                    could be INST_VAR_OF with a nesting level, so SSA
//                    construction had to decode meaning before it could number.
//   three-address    every instruction names its destination, so there is no
//                    evaluation stack to model.
//   fixed width      bci IS the instruction index, so the bci-to-machine map is
//                    an ARRAY. The old VM searched a descriptor Array linearly
//                    on every lookup.
//
// What is deliberately NOT here: arithmetic, at:, at:put: and size. They are
// sends, so the inline caches see them and the profile is real. The old VM
// resolved arithmetic inline at the call site, and when that fast path HIT it
// jumped over the inline cache entirely, so the profile at those sites recorded
// only the cases that MISSED. It was not imprecise, it was inverted.

#include "core/Object.h"
#include <stdint.h>

typedef enum {
	// -- movement and constants
	OP_MOVE,       // a := b
	OP_LOADK,      // a := literals[b]
	OP_LOADI,      // a := SmallInteger from the signed 16-bit b
	OP_LOADNIL,    // a := nil
	OP_LOADTRUE,   // a := true
	OP_LOADFALSE,  // a := false

	// -- instance state (not a message send in Smalltalk, so not a SEND)
	OP_GETIVAR,    // a := b's instance variable c
	OP_SETIVAR,    // a's instance variable b := c

	// -- globals: literals[b] is an Association
	OP_GETGLOBAL,  // a := value of literals[b]
	OP_SETGLOBAL,  // value of literals[a] := b

	// -- sends. Arguments live in registers b+1 .. b+n, consecutively.
	OP_SEND,       // a := send literals[b] to reg b_recv=c, n args after it
	OP_SENDSUPER,  // same, but lookup starts above the defining class

	// -- control. Targets are INSTRUCTION INDICES, not byte offsets.
	OP_JUMP,       // -> a
	OP_JUMPFALSE,  // if reg a is false -> b
	OP_JUMPTRUE,   // if reg a is true  -> b
	OP_GUARDCLASS, // if class index of reg a is not b -> c
	OP_RET,        // return reg a from this activation
	OP_RETOUTER,   // non-local return: return reg a from the block's home method

	// -- closures (ADR 0008: flat, with cells for captured-and-mutated)
	OP_CLOSURE,    // a := closure over blocks[b], capturing regs c .. c+n-1
	OP_GETUP,      // a := the running closure's captured slot b
	OP_SETUP,      // the running closure's captured slot a := b
	OP_NEWCELL,    // a := a fresh cell holding reg b
	OP_GETCELL,    // a := contents of the cell in reg b
	OP_SETCELL,    // contents of the cell in reg a := b

	// -- allocation. An instruction and not a send, because escape analysis has
	// to SEE the allocation in order to erase it.
	OP_NEW,        // a := instance of literals[b], fixed size
	OP_NEWIDX,     // a := instance of literals[b] with reg c elements

	// -- suspension point (ADR 0007). Loop back-edges and method entry.
	OP_SAFEPOINT,

	OP_COUNT
} Opcode;

// One instruction. The field order IS the encoding; nothing recomputes it.
typedef struct {
	uint8_t op;   // Opcode
	uint8_t n;    // arity for sends and closures, flags elsewhere
	uint16_t a;
	uint16_t b;
	uint16_t c;
} Instruction;

_Static_assert(sizeof(Instruction) == 8, "the bytecode encoding is 8 bytes wide");

// The 16-bit fields are real ceilings and every one of them fails LOUDLY.
// Precedent: the old VM's register allocator capped associations at a uint8
// index and SILENTLY ALIASED any literal past 255, which is the failure mode
// this constant exists to prevent.
#define BYTECODE_MAX_REGISTERS 65535
#define BYTECODE_MAX_LITERALS 65535
#define BYTECODE_MAX_INSTRUCTIONS 65535
#define BYTECODE_NO_TARGET 0xFFFF

// A declared type, for representation selection only (ADR 0006). Dispatch never
// consults these; they decide whether a value lives raw or tagged, and whether
// it crosses a call boundary raw or boxed.
typedef enum {
	DECL_NONE = 0, // undeclared: representation comes from the profile
	DECL_FLOAT64,
	DECL_INT64,
	DECL_BOOLEAN,
	DECL_CLASS,    // a declared class, index in CodeUnit.declaredClass[]
} DeclaredType;

// A compiled unit: a method or a block. Both have the same shape, which is why
// the tier-1 frame and the deopt map need only one layout.
typedef struct CodeUnit {
	Instruction *code;
	uint16_t instructionCount;
	uint16_t registerCount;
	uint16_t argumentCount;   // registers 1..argumentCount
	uint16_t captureCount;    // for a block: how many values it closes over

	// A BLOCK's register 0 holds the running closure; a METHOD's holds self.
	// That one difference is what CLOSURE reads to decide where a nested block's
	// home token comes from, and it cannot be inferred from captureCount, since
	// a block that captures nothing is still a block.
	_Bool isBlock;
	// This method has a block that does a NON-LOCAL RETURN somewhere inside it,
	// so its activations have to be findable by one (jit/Jit.c). Methods without
	// one pay nothing: no token is minted and no record is pushed.
	_Bool couldBeHome;

	// PrimitiveNumber, or PRIM_NONE. Compiled code attempts it right after the
	// prologue and returns its answer; on failure it falls through into the
	// bytecode below, which is the general case written in Smalltalk. Kept as a
	// plain uint16_t rather than the enum so this header does not have to know
	// the runtime's primitive set (runtime/Primitive.h).
	uint16_t primitive;

	Value literals;           // Array of literals, a GC root
	Value blocks;             // Array of CodeUnit-holding objects
	Value selector;
	Value ownerClass;

	// The SourceCode this unit was compiled from: WHERE the text is, not the
	// text itself (a file name plus a position and a length for a file parse,
	// the string itself for a string parse). Reflection over a method --
	// argument names, temporary names, the source a debugger shows -- reparses
	// through it, which is why nothing here has to keep a parse tree alive.
	Value source;
	// For a BLOCK, the CompiledMethod it was written inside; 0 for a method.
	// A block's code is a heap object like a method's, so `aBlock method` is a
	// field read rather than a search, and it is filled in AFTER the enclosing
	// method object exists (compiledMethodBindBlocks).
	Value homeMethod;
	// The CompiledMethod (or CompiledBlock) OBJECT whose code this unit is; 0
	// only for a unit no object was ever made for. It is what turns a native
	// FRAME back into something the image can hold: a materialised Context's
	// `code` is this field, read off the frame's NativeCode->unit
	// (runtime/primitives/Context.c). Filled where the object is created
	// (jit/CompiledMethod.c codeCreate).
	Value codeObject;

	// Parallel to `code`: source position per instruction, for backtraces.
	// A side table and not a field of Instruction, because it is cold.
	uint32_t *sourcePositions;

	// Parallel to the registers: the declared type of each, DECL_NONE when
	// undeclared. This is the whole footprint of the type system on the
	// bytecode, and it is metadata rather than encoding on purpose, so the type
	// system can grow without the opcode set moving.
	uint8_t *declaredTypes;
	uint16_t *declaredClass;  // class index when declaredTypes[i] == DECL_CLASS
	uint8_t returnType;

	// The unit registry (jit/Jit.h, jitRegisterUnit). A unit is a malloc'd C
	// struct holding TAGGED VALUES, so nothing else in the system can find them
	// and a young collection moves precisely what they name. Chaining the units
	// themselves is what makes the literal frame a root from the moment it
	// exists rather than from the first call, which is what a block unit needs:
	// it is built when its enclosing method compiles and does not run until
	// someone sends it `value`, and a collection in between is ordinary.
	struct CodeUnit *nextUnit;
	_Bool registered;
} CodeUnit;


static inline const char *opcodeName(Opcode op)
{
	static const char *names[OP_COUNT] = {
		"MOVE", "LOADK", "LOADI", "LOADNIL", "LOADTRUE", "LOADFALSE",
		"GETIVAR", "SETIVAR", "GETGLOBAL", "SETGLOBAL",
		"SEND", "SENDSUPER",
		"JUMP", "JUMPFALSE", "JUMPTRUE", "GUARDCLASS", "RET", "RETOUTER",
		"CLOSURE", "GETUP", "SETUP", "NEWCELL", "GETCELL", "SETCELL",
		"NEW", "NEWIDX",
		"SAFEPOINT",
	};
	return op < OP_COUNT ? names[op] : "?";
}


// Does this instruction end a basic block? The CFG builder asks nothing else.
static inline _Bool opcodeIsTerminator(Opcode op)
{
	return op == OP_JUMP || op == OP_JUMPFALSE || op == OP_JUMPTRUE
		|| op == OP_GUARDCLASS || op == OP_RET || op == OP_RETOUTER;
}


// Where a branch can go, besides falling through. BYTECODE_NO_TARGET when it
// cannot branch at all.
static inline uint16_t opcodeBranchTarget(const Instruction *instruction)
{
	switch ((Opcode) instruction->op) {
	case OP_JUMP:
		return instruction->a;
	case OP_JUMPFALSE:
	case OP_JUMPTRUE:
		return instruction->b;
	case OP_GUARDCLASS:
		return instruction->c;
	default:
		return BYTECODE_NO_TARGET;
	}
}


// Does control fall through to the next instruction?
static inline _Bool opcodeFallsThrough(Opcode op)
{
	return op != OP_JUMP && op != OP_RET && op != OP_RETOUTER;
}


// Every send site carries an inline cache, and the cache is reached BY BCI.
// Indexing sites by instruction index rather than by a dense send number costs
// one word per non-send instruction and buys O(1) lookup from exactly the
// coordinate the tier, the deopt map and OSR all already carry.
static inline _Bool opcodeIsSend(Opcode op)
{
	return op == OP_SEND || op == OP_SENDSUPER;
}

#endif
