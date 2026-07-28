#ifndef COMPILE_H
#define COMPILE_H

// The front end from the syntax tree down: name resolution, then bytecode.
//
// The boundary the project drew (docs/jit-v2/03-escopo-revisado.md) is SYNTAX
// STAYS, SEMANTICS IS NEW. Tokenizer, Parser and Ast are the old ones; this
// file and Compile.c are what replaces Scope.c and Compiler.c, because the
// bytecode they targeted is gone and the one they target now is designed for
// SSA construction and for deoptimization rather than for direct interpretation
// (docs/jit-v2/04-bytecode.md).
//
// Three phases, in one translation unit because each consumes the one before it
// name by name and a header between them would only describe the join:
//
//   CAPTURE      which names each non-inlined block closes over, and which
//                declarations need a heap CELL because they are captured AND
//                assigned (ADR 0008). It runs over the whole method first,
//                because a cell changes every read and write of a variable,
//                including the ones emitted before the closure exists;
//   RESOLUTION   every identifier becomes a place: self, an argument register,
//                a temporary register, a cell, a capture slot, an
//                instance-variable slot, or a global Association. Registers are
//                FLAT, so a resolved local IS an SSA value later, with nothing
//                to interpret first;
//   EMISSION     the tree becomes fixed-width instructions.
//
// WHAT IS DELIBERATELY NOT DONE HERE (ADR 0006, and it is the discipline the
// whole project rests on): arithmetic, at:, at:put: and size are emitted as
// ORDINARY SENDS. They are not folded, not special-cased and not resolved
// against a known receiver class. The old compiler was already right about
// this and its code generator was not; keeping it right here is what makes the
// inline caches see the hot sites at all.

#include "compiler/Bytecode.h"
#include "compiler/Ast.h"
#include "core/Handle.h"
#include "runtime/Dictionary.h"

typedef enum {
	COMPILE_OK,
	COMPILE_UNDECLARED_NAME,
	COMPILE_TOO_MANY_REGISTERS,
	COMPILE_TOO_MANY_LITERALS,
	COMPILE_TOO_MANY_INSTRUCTIONS,
	COMPILE_TOO_MANY_CAPTURES,  // a block closing over more than a CLOSURE can name
	COMPILE_BAD_INLINE_BLOCK,   // a block where the shape required one
	COMPILE_UNKNOWN_PRIMITIVE,  // <primitive: Foo> naming nothing the VM knows
	COMPILE_UNSUPPORTED,        // reached a construct this stage does not do yet
} CompileStatus;

typedef struct {
	CompileStatus status;
	// The offending name or selector, for the message. A HANDLE, valid only
	// while the caller's scope is open.
	String *what;
} CompileError;

// What a method is compiled AGAINST: the class supplying instance variables and
// the superclass chain, and the namespace supplying globals.
typedef struct {
	Class *ownerClass;   // may be NULL for a bare expression
	Dictionary *globals; // Symbol -> Association
	// Where CLASS VARIABLES are looked up, which is NOT always ownerClass.
	//
	// A class variable is shared by a class, its subclasses AND its metaclass:
	// `Character class initialize` assigns Table and `Character>>isVowel` reads
	// it, and they have to reach the same Association. But a class-side method is
	// compiled with the METACLASS as ownerClass, and a metaclass holds none of
	// the class's class variables, so searching ownerClass answers nothing for
	// the class side.
	//
	// Getting this wrong is silent, not loud: the name starts with a capital, so
	// the unresolved write lands on a freshly minted forward-reference GLOBAL of
	// the same name, and the instance-side read keeps seeing nil.
	//
	// NULL means "same as ownerClass", which is right for instance-side methods
	// and for a bare expression.
	Class *classVariableScope;
} CompileContext;

// Compile one parsed method. Answers a malloc'd CodeUnit, or NULL with `error`
// filled in.
//
// The unit's literal frame is a heap Array and the unit itself is a C struct, so
// the caller owns both: the Array has to stay reachable, which for a real method
// means the CompiledMethod that wraps it (see the note on rootsVisitCompiledCode
// in jit/Jit.c).
CodeUnit *compileMethod(MethodNode *method, const CompileContext *context,
	CompileError *error);

const char *compileStatusName(CompileStatus status);

// Print a unit's instructions, one per line. The bci is the index, so what this
// prints is exactly the coordinate the deopt map and the bci-to-machine map use.
void codeUnitPrint(const CodeUnit *unit);

#endif
