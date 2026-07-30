#ifndef DEOPT_H
#define DEOPT_H

// Deoptimization: leaving optimized code and continuing in the tier-1 frame
// that the optimized code was derived from.
//
// The rule that decides correctness, and getting it wrong is a DOUBLE CALL that
// is silent and awful to find:
//
//   the INNERMOST frame RE-EXECUTES the instruction that failed;
//   every OUTER frame already had its call in flight, so it resumes at the
//   instruction AFTER the send, with the result deposited in its destination
//   register.
//
// Resuming the outer frame at the send instead would call the same method a
// second time. If the method allocated, or wrote a field, or printed, the
// program is wrong in a way that no assertion sees.

#include "compiler/Bytecode.h"
#include "core/Object.h"
#include "jit/Ir.h"
#include "memory/Roots.h"

// ---------------------------------------------------------------------------
// THE STATE AT RUN TIME, WHICH IS NOT THE STATE AT COMPILE TIME
// ---------------------------------------------------------------------------
//
// DeoptState above names `IrValue *`: pointers into the IR arena, which is
// freed the moment compilation ends. It is a description of the state in terms
// the OPTIMIZER understands, and it is unusable from running code -- not
// inconvenient, unusable, because the memory is gone.
//
// So the backend translates it, once, into terms the RUNTIME understands:
// for each live bytecode register, WHERE its value is in the optimized frame.
// That translation can only happen after register allocation, because before
// then there is no answer, and it must happen before the LIR is destroyed,
// because after then the question cannot be asked.
//
// This is the same shape of mistake the FrameMap avoided by existing before the
// JIT did: a description the producer can write and the consumer cannot read is
// not a description.

typedef enum {
	DEOPT_IN_REGISTER,  // a physical register of the optimized frame
	DEOPT_IN_SLOT,      // a frame slot of the optimized frame
	DEOPT_CONSTANT,     // known at compile time and in neither
} DeoptWhere;

typedef struct {
	uint16_t bytecodeRegister; // which tier-1 register this reconstructs
	uint8_t where;             // DeoptWhere
	uint8_t kind;              // SlotKind: raw values are re-boxed on the way out
	uint8_t bank;              // LirBank, for DEOPT_IN_REGISTER
	int32_t location;          // physical register number, or frame slot index
	Value constant;            // DEOPT_CONSTANT
} DeoptSlot;

struct NativeCode;

// How many physical registers the guard sequence spills before calling out, one
// per register number the backend can hand out, PER BANK.
//
// IN THE FRAME and not in a thread-local, and the reason is mechanical rather
// than a preference: a __thread address is not a link-time constant, so emitted
// code cannot bake one without also knowing the platform's TLS model. The frame
// it is already standing in needs no such knowledge.
//
// BOTH BANKS, and the integer half alone was a wrong answer waiting for the
// first float value to live in a register. A DeoptSlot records the bank it was
// allocated in, and reading one back out of a save area that only ever held
// integer registers would take the eight bytes of whatever integer register
// happens to share the number -- a plausible double, from a completely
// unrelated value. Nothing produced float-bank values before arithmetic
// specialization existed, which is exactly why it stayed invisible.
#define DEOPT_SAVED_REGISTERS 16
// The integer bank first, then the float bank, so register r of bank b is at
// saveBase + b * DEOPT_SAVED_REGISTERS + r. Spelled here and read by the
// emitter and by DeoptResume.c, because two layouts that have to agree are one
// layout or they are a bug.
#define DEOPT_SAVE_SLOTS (2 * DEOPT_SAVED_REGISTERS)

typedef struct {
	CodeUnit *unit;
	// The TIER-1 compilation to resume into. Carried rather than derived: a
	// CodeUnit does not name its native code, the CompiledMethod does, and the
	// method is not reachable from here.
	struct NativeCode *baseline;
	uint16_t bci;
	uint16_t destRegister;
	_Bool innermost;
	uint16_t slotCount;
	DeoptSlot *slots;
	// Where in the optimized frame the guard spilled the physical registers.
	// Recorded per site rather than derived, so reading one back never depends
	// on recomputing a layout the emitter already decided.
	uint16_t saveBase;
} DeoptRuntimeFrame;

// Everything needed to leave optimized code at ONE point in it.
//
// Keyed by CODE OFFSET, the same coordinate the frame maps use, because the
// only thing running code knows about where it is is its own return address.
typedef struct DeoptSite {
	uint32_t codeOffset;
	uint16_t frameCount;
	DeoptRuntimeFrame *frames;
} DeoptSite;

// Rebuild an object that escape analysis erased. Called at the moment a guard
// fails, with the field values read out of the optimized frame.
//
// This ALLOCATES, and therefore may collect, with the whole deoptimization
// state live. Everything the state names has to be reachable at that moment,
// which is why the values arrive in an array the caller keeps rooted rather
// than as raw pointers pulled from a frame the collector cannot see.
Value deoptMaterialize(uint32_t classIndex, _Bool flat, Value *fields,
	uint16_t fieldCount);

// Is every value a deoptimization state names still resolvable? A state that
// mentions a value the optimizer deleted is the bug this catches, and it can
// only be caught here: at run time it presents as a wrong answer on the path
// nobody exercises.
_Bool deoptStateIsWellFormed(const DeoptState *state);

// Every guard fails. The stress mode the plan requires (phase 3): with it on,
// every test and every benchmark must produce results IDENTICAL to a normal
// run. If that passes, the deoptimization machinery is correct BEFORE any
// optimization is built on top of it, which is the whole reason phase 3 comes
// before phase 4.
_Bool deoptStressEnabled(void);

// How many send sites --deopt-stress leaves alone before its first guard, from
// ST_DEOPT_STRESS_SKIP. Zero unless asked.
//
// It exists because only the FIRST guard a path reaches ever fires: after it the
// rest of that activation is tier 1's. One run therefore stresses the first site
// of every method, and sweeping this walks the stress deeper into them, which is
// the difference between a claim and a measured one.
uint16_t deoptStressSkip(void);

// A guard failed: rebuild the tier-1 frame from `site` and continue in it.
// Answers what the method answers, because the resumed tier-1 code returns
// straight past this. See DeoptResume.c for why it stacks a frame rather than
// replacing one.
Value jitDeoptimize(void *site, Value *slotZero, uint64_t packed);

// How many times optimized code has been left this way.
//
// IT EXISTS BECAUSE THE ANSWER CANNOT BE CHECKED. Deoptimizing is correct by
// construction: a guard that fails on every single execution still produces
// exactly the answer tier 1 produces, so a differential test cannot tell a
// working speculation from one that never holds -- and "never holds" is the
// failure mode of a guard that mis-decodes a class, which is a total loss of
// everything this tier is for, reported as success. This counter is what makes
// that visible, and the check that reads it is "an in-profile call deoptimizes
// ZERO times".
uint64_t jitDeoptimizationCount(void);

// Enter compiled code at `target` with a prebuilt register file, `registers[i]`
// becoming frame slot i. Answers what that code answers.
//
// The ONE piece of this that names a register, so it is implemented per ABI in
// vm/jit/<arch>/abi/<abi>/ and only declared here (ADR 0009).
Value jitResumeAt(const void *target, const Value *registers, uint64_t count,
	uint64_t frameBytes);

#endif
