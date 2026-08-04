#ifndef SSA_EMITTER_H
#define SSA_EMITTER_H

// The SSA backend's emission seam: allocated LIR to machine code.
//
// A SECOND ops-struct, beside jit/MacroAssembler.h, and the two are at
// DIFFERENT LEVELS on purpose (ADR 0009). The macro assembler's vocabulary is
// bytecode-shaped -- "load frame slot i", "send" -- which is right for a
// template compiler where every sequence begins and ends in memory. This one is
// machine-shaped: "add these two registers". Tier 2 needs the second because it
// keeps values in registers, and putting those words into the macro assembler
// would drag register allocation into the tier whose correctness argument is
// that it has none.
//
// The rule that has not changed: NO REGISTER NAME OUTSIDE vm/jit/<arch>/. The
// driver below walks blocks and instructions and never learns what a register
// is; the backend's `instruction` hook is a switch that does.
//
// A VTABLE and not #ifdef, for the reason the macro assembler already gives:
// link-time selection makes exactly one backend exist per binary, which
// forecloses emitting for a FOREIGN target on the current host and comparing
// bytes against an oracle. With an ops struct that is an ordinary test.

#include "jit/Lir.h"

typedef struct SsaEmitter SsaEmitter;
typedef struct SsaLabel SsaLabel;

typedef struct SsaEmitterOps {
	const char *name;
	const Abi *abi;
	size_t stateBytes;   // per-instance backend scratch

	void (*begin)(SsaEmitter *);
	void (*end)(SsaEmitter *);
	// The frame, the incoming arguments and the callee-saved registers the
	// allocation used. Everything it needs is on the LirFunction.
	void (*prologue)(SsaEmitter *, Value nilValue);

	SsaLabel *(*newLabel)(SsaEmitter *);
	void (*bind)(SsaEmitter *, SsaLabel *);
	void (*jump)(SsaEmitter *, SsaLabel *);

	// One instruction. `taken` and `notTaken` are the successors' labels, and
	// are NULL for anything that is not a terminator.
	//
	// The BRANCH STRUCTURE is the backend's, which is why the labels are passed
	// rather than the driver emitting the jumps around a condition: how a
	// compare and a conditional branch fit together differs per target, and a
	// driver that owned it would be naming machine-level facts again.
	void (*instruction)(SsaEmitter *, const LirInstruction *, SsaLabel *taken,
		SsaLabel *notTaken);
} SsaEmitterOps;

// Every SSA backend compiled into this build, NULL-terminated.
extern const SsaEmitterOps *const gSsaEmitterBackends[];
const SsaEmitterOps *ssaHostBackend(void);

// Emit `function`, which must have been through lirAllocateRegisters with no
// failure. Answers the emitter, whose buffer holds the bytes; every
// instruction's codeOffset is filled in on the way, so the frame maps can be
// anchored to machine offsets afterwards.
SsaEmitter *ssaEmit(const SsaEmitterOps *ops, LirFunction *function,
	Value nilValue);
void ssaEmitterDestroy(SsaEmitter *emitter);

const uint8_t *ssaEmitterBytes(SsaEmitter *emitter, size_t *size);

// Backend-private reach-throughs.
struct CodeBuffer *ssaEmitterBuffer(SsaEmitter *emitter);
void *ssaEmitterState(SsaEmitter *emitter);
LirFunction *ssaEmitterFunction(SsaEmitter *emitter);
const Abi *ssaEmitterAbi(SsaEmitter *emitter);

#endif
