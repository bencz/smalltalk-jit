#ifndef STUBCODE_H
#define STUBCODE_H

#include "CompiledCode.h"
#include "CodeGenerator.h"
#include "Assembler.h"
#include "Thread.h"

// Index into Heap.stubCode[] — must stay in [0, STUB_COUNT) (Heap.h).
typedef enum {
	STUB_SMALLTALK_ENTRY,
	STUB_ALLOCATE,
	STUB_LOOKUP,
	STUB_DNU,
} StubId;

// A stub DESCRIPTOR: pure (isolate-independent) — just how to generate the stub and
// which per-heap slot caches the result. The generated NativeCode lives in
// Heap.stubCode[id], shared by all mutators of that heap (not in TLS).
typedef struct {
	void (*generator)(CodeGenerator *generator);
	StubId id;
} StubCode;

extern StubCode SmalltalkEntry;
extern StubCode AllocateStub;
extern StubCode LookupStub;
extern StubCode DoesNotUnderstandStub;

NativeCode *getStubNativeCode(StubCode *stub);
void generateStubCall(CodeGenerator *generator, StubCode *stubCode);

#endif
