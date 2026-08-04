#include "jit/MacroAssembler.h"

// Every backend compiled into this build.
//
// A LIST and not a link-time choice: with all of them present, emitting for a
// foreign target is an ordinary function call, and a cross-emission test can
// compare the bytes a ppc64 backend produces against an oracle while running on
// x86-64, with no emulator involved. That capability is why ADR 0009 chose an
// ops struct over #ifdef.

// One entry per (architecture, ABI). The emitter is shared per architecture and
// lives in vm/jit/<arch>/; the ABI tables live in vm/jit/<arch>/abi/<abi>/,
// one file each, because that is the granularity at which platforms differ.
extern const MacroAssemblerOps gMacroAssemblerX64SysV;
extern const MacroAssemblerOps gMacroAssemblerX64Win64;

const MacroAssemblerOps *const gMacroAssemblerBackends[] = {
	&gMacroAssemblerX64SysV,
	&gMacroAssemblerX64Win64,
	// PENDING, each one emitter plus one table per ABI:
	//   vm/jit/ppc64/   + abi/elfv1/ (big-endian Linux), abi/aix/
	//   vm/jit/ppc64le/ + abi/elfv2/
	//   vm/jit/arm64/   + abi/aapcs/, abi/darwin/, abi/win/
	NULL,
};
