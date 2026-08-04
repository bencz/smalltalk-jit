#ifndef SSA_BUILD_H
#define SSA_BUILD_H

// Bytecode to SSA, in one pass, by the algorithm of Braun et al.
//
// Chosen over dominance frontiers for two reasons: it is far shorter, and it
// builds SSA DURING the lowering walk rather than needing a separate analysis
// first. The register bytecode is what makes it trivial: a bytecode register IS
// a variable of the algorithm, with no operand to interpret before numbering.
//
// Every guard, send and safepoint is born carrying its DEOPTIMIZATION STATE.
// That ordering is not incidental. A state attached after the optimizer has run
// describes a frame that no longer exists, which is why the deoptimization
// infrastructure comes before the optimizer rather than after it.

#include "compiler/Bytecode.h"
#include "jit/Ir.h"

// Answers NULL when the unit uses an opcode this builder does not model, and
// says which through `unsupported`. Same contract as jitCompile, and for the
// same reason: an opcode nobody taught it would otherwise be skipped in silence.
IrFunction *ssaBuild(CodeUnit *unit, Opcode *unsupported);

#endif
