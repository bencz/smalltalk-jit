#ifndef TRAITS_PPC64LE_H
#define TRAITS_PPC64LE_H

// ppc64 (LITTLE-ENDIAN, ELFv2) target traits — see jit/TargetTraits.h for the
// contract. big-endian ppc64 is a SEPARATE backend (vm/jit/ppc64/): the ecosystem
// treats them as distinct architectures and the codegen diverges beyond the
// ABI (function descriptors vs localentry). Shared POWER encoding helpers can
// be factored into a common header when the real encoders land.

// All-zero words decode as an illegal instruction on POWER, so zero-filled
// executable pages trap a stray jump. (No single repeated byte forms a "nice"
// trap like x86's INT3 on a fixed-width ISA; 0x00 is the honest choice.)
#define TARGET_CODE_FILLER_BYTE 0x00

#define TARGET_STACK_GROWS_DOWN 1

// The C-side thread pointer: r13 on ppc64 (some gcc versions reject
// __builtin_thread_pointer for this target, so read the register directly).
// PORT_ME(tls): the ppc64 TLS ABI biases thread-pointer-relative offsets by
// 0x7000 — re-derive the tpoff contract before implementing emitLoadTls.
static inline void *targetThreadPointer(void)
{
	register void *tp __asm__("r13");
	__asm__("" : "=r"(tp));
	return tp;
}

#endif
