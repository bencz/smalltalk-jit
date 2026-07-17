#ifndef CODE_GENERATOR_H
#define CODE_GENERATOR_H

#include "core/Object.h"
#include "core/CompiledCode.h"
#include "jit/Assembler.h"
#include "jit/TargetAssembler.h"
#include "core/Lookup.h"
#include "runtime/String.h"
#include "jit/RegisterAllocator.h"

// Tagged so ABI instance TUs can hold a CodeGenerator* OPAQUELY (they must
// not include this header: on a foreign golden host it would drag in the
// HOST backend's TargetAssembler; see the Ppc64Abi emitCCallPrimitive hook).
typedef struct CodeGenerator {
	CompiledCode code;
	AssemblerBuffer buffer;
	size_t frameSize;
	size_t frameRawAreaSize;
	RegsAlloc regsAlloc;
	uint8_t tmpVar;
	ptrdiff_t bytecodeNumber;
	OrderedCollection *stackmaps;
	OrderedCollection *descriptors;
	// When set, generateStackmap over-approximates liveness (marks every spilled
	// temp whose range starts at/before the current bytecode, ignoring its end).
	// Used only for a loop back-edge safepoint poll, where the control-flow-unaware
	// linear-scan liveness would otherwise omit a loop-carried, body-only pointer
	// that is live into the next iteration — see generateSafepointPoll.
	_Bool overapproxStackmap;
	// Tier-1 recompilation (jit/Tier.h): the superseded NativeCode whose IC
	// cells feed the speculative send promotion, or NULL for a plain tier-0
	// compile. Non-NULL also suppresses the invocation-counter check (a method
	// recompiles exactly once).
	NativeCode *tierFeedback;
	// When the tier-1 method was REWRITTEN by the inliner (compiler/
	// Optimizer.c), the positional send-to-cell pairing no longer holds:
	// this map, indexed by bytecode instruction number of the rewritten
	// method, carries each send's feedback cell (NULL = none). NULL map =
	// original bytecodes = positional pairing.
	IcCell **tierSiteMap;
	size_t tierSiteMapSize;
} CodeGenerator;

// Neutral initializer (jit/StubCode.c) — fresh buffer + zeroed frame state.
void initCodeGenerator(CodeGenerator *generator);

NativeCode *generateMethodCode(CompiledMethod *method);
NativeCode *generateMethodCodeTiered(CompiledMethod *method, NativeCode *feedback,
	IcCell **siteMap, size_t siteMapSize);
void generateLoadObject(AssemblerBuffer *buffer, RawObject *object, Register dst, _Bool tag);
void generateLoadClass(AssemblerBuffer *buffer, Register src, Register dst);
void generateStoreCheck(CodeGenerator *generator, Register object, Register value);
void generateIcGuard(AssemblerBuffer *buffer, AssemblerLabel *miss);
void generateMethodLookup(CodeGenerator *generator);
void generateStackmap(CodeGenerator *generator);
void generateCCall(CodeGenerator *generator, intptr_t cFunction, size_t argsSize, _Bool storeIp);
void generateMethodContextAllocation(CodeGenerator *generator, size_t size);
void generateBlockContextAllocation(CodeGenerator *generator);
void generatePushDummyContext(AssemblerBuffer *buffer);
NativeCode *generateDoesNotUnderstand(String *selector);
NativeCode *buildNativeCode(CodeGenerator *generator);
NativeCode *buildNativeCodeFromAssembler(AssemblerBuffer *buffer);

#endif
