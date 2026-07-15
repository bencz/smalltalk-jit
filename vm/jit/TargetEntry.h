#ifndef TARGET_ENTRY_H
#define TARGET_ENTRY_H

// The ONE place C code invokes JIT-generated code: calling the
// SmalltalkEntry stub (vm/core/Entry.c). On most ABIs that is a plain cast
// of the stub's insts to a C function pointer — but under ppc64 ELFv1 a C
// function pointer is a FUNCTION DESCRIPTOR pointer {entry, TOC, environ},
// so the call must go through a synthesized descriptor instead of the raw
// code address. Link-time bound: x64 in vm/jit/x64/Abi.c, ppc64/elfv1 in
// AbiElfV1Bind.c. Everything else in the VM (lookup cache entries, send
// targets, NativeCode.insts) stays a RAW code address — JIT-to-JIT calls
// never use descriptors.
//
// arg0/arg1 mirror the stub's first two C arguments (method|compiledCode,
// native-code entry) — see generateSmalltalkEntry.
#include "core/Object.h"

struct Thread;

Value targetCallSmalltalkEntry(void *entryStubInsts, void *arg0, void *arg1,
	Value *args, struct Thread *thread);

#endif
