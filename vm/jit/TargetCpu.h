#ifndef TARGET_CPU_H
#define TARGET_CPU_H

// The CPU-MODEL axis: which microarchitecture of the selected instruction set
// this PROCESS is running on. It is the fourth binding axis, and the only one
// that cannot be resolved at build time:
//
//   ST_ARCH   link-time     which instruction set        vm/jit/<arch>/
//   ST_ABI    emit-time     which C calling convention   vm/jit/<arch>/abi/<abi>/
//   traits    compile-time  constants generic C needs    jit/TargetTraits.h
//   THIS      RUNTIME       which CPU MODEL of that arch vm/jit/<arch>/Cpu.h
//
// One ppc64le binary must run on POWER8 and on POWER10; one x64 binary on Zen
// and on Skylake. So the feature set is only knowable after startup, which is
// why this cannot be a #if, a CMake file choice or a trait.
//
// Contract:
//  - targetCpuDetect() runs ONCE, as the first thing main() does, BEFORE any
//    code is generated (note that main.c's self-test dispatch returns early,
//    so detection has to precede even that).
//  - After it returns, the arch's feature struct (gX64Cpu / gPpc64Cpu /
//    gPpc64leCpu, declared in vm/jit/<arch>/Cpu.h) is read-only for the rest of
//    the process. It is therefore safe to read from every worker thread with no
//    locking and no TLS.
//  - It is consulted at EMIT time only. Generated code NEVER tests a feature
//    flag: the JIT picks the instruction sequence while emitting, exactly like
//    the ABI vtable, so tuned code pays zero runtime cost.
//  - Detection must be CONSERVATIVE: the default is the ISA baseline the C
//    compiler was already told to assume for this build, so a failed or absent
//    detection can only lose optimizations, never emit an illegal instruction.
//
// No JIT-generated code is persisted (the image stores bytecode; native code is
// regenerated every run, which is why the bootstrap image is byte-count
// identical across x64/ppc64/ppc64le), so CPU-tuned code creates NO
// image-portability hazard: an image built on POWER10 runs fine on POWER8.
//
// Knobs (both read by targetCpuDetect):
//   ST_CPU=<name>   force a feature set instead of detecting, so a tuned path
//                   can be emitted and inspected from a machine that does not
//                   have that CPU. Names are per-arch; ST_CPU=? lists them.
//   ST_CPU_INFO=1   print what was detected (or forced) and continue.
void targetCpuDetect(void);

#endif
