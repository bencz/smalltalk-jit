#ifndef STUBCODE_H
#define STUBCODE_H

#include "CompiledCode.h"
#include "CodeGenerator.h"
#include "Assembler.h"
#include "Thread.h"

typedef struct {
	void (*generator)(CodeGenerator *generator);
	NativeCode *nativeCode;
} StubCode;

extern PER_ISOLATE StubCode SmalltalkEntry;
extern PER_ISOLATE StubCode AllocateStub;
extern PER_ISOLATE StubCode LookupStub;
extern PER_ISOLATE StubCode DoesNotUnderstandStub;

NativeCode *getStubNativeCode(StubCode *stub);
void generateStubCall(CodeGenerator *generator, StubCode *stubCode);

#endif
