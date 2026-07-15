#ifndef TRAITS_X64_H
#define TRAITS_X64_H

// x86-64 target traits (see jit/TargetTraits.h for the contract).

// INT3: a stray jump into unwritten executable memory traps immediately
// instead of sliding.
#define TARGET_CODE_FILLER_BYTE 0xCC

#define TARGET_STACK_GROWS_DOWN 1

// The C-side thread pointer (TLS base), matching what the ABI's emitLoadTls
// reaches at runtime. gcc/clang support the builtin on x86-64 (%fs base).
static inline void *targetThreadPointer(void)
{
	return __builtin_thread_pointer();
}

#endif
