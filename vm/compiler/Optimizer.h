#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "core/CompiledCode.h"

// Tier-1 speculative inliner (bytecode level), driven by the IC cells of the
// superseded NativeCode. Answers a FRESH CompiledMethod whose monomorphic
// sends with eligible leaf callees were rewritten as
//   JUMP_NOT_MEMBER_OF hintClass, receiver -> fallback
//   <receiver/arg spills> <callee body, operands remapped>  JUMP -> done
//   fallback: <the original send, verbatim>   done:
// or NULL when nothing was inlined (the caller then compiles the ORIGINAL
// method with the positional site feedback, exactly the tier M1 path).
//
// *siteMapOut (malloc'd, caller frees; sized *siteMapSizeOut = instruction
// count of the new method) maps each instruction number of the NEW method to
// the old-code IC cell feeding its promotion, or NULL: sends copied from the
// original keep their cell (the codegen's direct-call promotion still
// applies), sends synthesized from callee bodies and the fallback sends of
// inlined sites carry no feedback.
CompiledMethod *optimizeMethod(CompiledMethod *method, NativeCode *oldCode,
	IcCell ***siteMapOut, size_t *siteMapSizeOut);

// Test-only probe (vm/tests/SelfTests.c): would `callee` be inlined with
// `receiverClass` as the guarded exact class? Exposes the eligibility filter
// so its rejections (primitive, context, outer returns, jumps, size) are
// directly assertable.
_Bool optimizerInlineEligibleForTest(CompiledMethod *callee, Class *receiverClass);

#endif
