#ifndef ABI_H
#define ABI_H

// The platform C ABI, as DATA rather than as code.
//
// One instance per (architecture, ABI) pair: x64+sysv, x64+win64, ppc64+elfv1,
// ppc64le+elfv2, arm64+aapcs. Everything that differs between them and that a
// compiler needs to know lives here, so a port supplies a table rather than
// rewriting a code generator.
//
// The register numbers are BACKEND-LOCAL: only the backend that defines an ABI
// knows what number 7 means. Nothing above the backend interprets them, which
// is what lets this header be arch-neutral while describing arch-specific
// facts.

#include <stdint.h>

typedef struct {
	const char *name;

	// Integer argument registers, in order. A call with more arguments than
	// this passes the rest on the stack, per the ABI's own rules.
	const uint8_t *argumentRegisters;
	uint8_t argumentRegisterCount;
	const uint8_t *floatArgumentRegisters;
	uint8_t floatArgumentRegisterCount;

	uint8_t integerResult;
	uint8_t floatResult;

	// What a callee must preserve. The SSA backend's allocator reads this to
	// decide what it may use across a call without spilling; the fiber switch
	// reads it to know what a context switch has to carry.
	//
	// PER BANK, because the two number independently: register 6 is RSI in the
	// integer file and XMM6 in the float file, and under Win64 one of those is
	// callee-saved and the other is not. One list could not say that, and the
	// allocator asking it would get an answer about the wrong file.
	//
	// An EMPTY float list is a real answer and not an unfinished one: under
	// System V every SSE register is caller-saved, so a double never survives a
	// call there.
	const uint8_t *calleeSaved;
	uint8_t calleeSavedCount;
	const uint8_t *callerSaved;
	uint8_t callerSavedCount;
	const uint8_t *calleeSavedFloat;
	uint8_t calleeSavedFloatCount;

	// Registers the allocator may hand out, excluding the stack and frame
	// pointers and whatever the macro assembler reserves for itself.
	const uint8_t *allocatableInteger;
	uint8_t allocatableIntegerCount;
	const uint8_t *allocatableFloat;
	uint8_t allocatableFloatCount;

	// Stack pointer alignment required AT A CALL INSTRUCTION. SysV and AAPCS
	// say 16; getting it wrong shows up as a crash inside the first SSE or
	// NEON instruction the callee happens to use, which reads like anything
	// but a stack bug.
	uint16_t stackAlignment;
	// Bytes the caller must reserve for the callee to spill its register
	// arguments into. 32 on Win64, 0 on SysV, and the reason this is a field
	// rather than an #ifdef somewhere.
	uint16_t shadowSpaceBytes;
	// Bytes reserved below the stack pointer that a signal handler will not
	// disturb, and which a leaf function may therefore use without adjusting
	// the stack pointer. 128 on SysV x86-64, 0 almost everywhere else.
	uint16_t redZoneBytes;
} Abi;

// The ABI this build targets, bound at link time by exactly one backend file.
extern const Abi *gAbi;

#endif
