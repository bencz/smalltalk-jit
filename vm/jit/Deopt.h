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

#endif
