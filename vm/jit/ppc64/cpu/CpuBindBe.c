// The big-endian CPU-model bind: the process-wide feature struct at ITS
// baseline, plus the ST_CPU names this target accepts. Picked by CMake next
// to the ABI bind (elfv1); the pure decode in CpuPpc64.c is shared by both
// targets and by the host goldens, which link NO bind.
//
// The BE BASELINE is the PowerPC 64-bit base ISA, which is what the JIT is
// measured to emit (see Cpu.h). Claiming nothing means a failed, absent or
// under-reporting detection can only lose optimizations, never emit an
// illegal instruction, and it is the level an Apple G5 runs.
#if !defined(__powerpc64__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "CpuBindBe.c is for BIG-ENDIAN ppc64 builds - check the CPU bind selection in CMakeLists.txt"
#endif

#include "jit/ppc64/Cpu.h"

Ppc64Cpu gPpc64Cpu = {
	.name = PPC64_CPU_BASELINE_NAME,
};

// Every profile the decoder knows is a valid BE target.
const char *const *const Ppc64CpuAccepted = Ppc64CpuNames;
