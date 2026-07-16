// Golden-byte emission tests for the x86-64 backend's ABI-sensitive emitters
// (ST_ABI_EMIT_TEST=1). Each sequence is emitted through the REAL generators
// into an AssemblerBuffer and compared byte-for-byte against expected vectors
// captured from a known-good build. This is what pins the ABI seam refactors
// to today's exact machine code, and what lets a FUTURE foreign ABI
// (abi/win64/, ppc64 elfv1/elfv2) be developed on this host by golden-testing
// its emitters without the target hardware.
//
// Determinism: all pointer-shaped immediates are fixed fakes (FAKE_C_FUNCTION,
// FAKE_TPOFF); struct offsets (offsetof(Thread,...)) are compile-time and only
// change when the structs change — in which case regenerating the vectors is
// the correct response.
//
// Maintenance: ST_ABI_EMIT_TEST=print hexdumps every sequence as a paste-ready
// C array; regenerate, eyeball the diff, paste, commit.
#ifndef __x86_64__
#error "EmitGoldenX64.c is x86-64-only - wire the arch's own golden TU in CMake"
#endif

#include "vm/tests/SelfTests.h"
#include "vm/jit/CodeGenerator.h"
#include "vm/jit/StubCode.h"
#include "vm/jit/TargetPrimitives.h"
#include "vm/jit/x64/AssemblerX64.h"
#include "vm/jit/x64/Cpu.h"
#include "vm/jit/x64/Abi.h"
#include "vm/core/Thread.h"
#include "vm/core/Lookup.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define FAKE_C_FUNCTION ((intptr_t) 0x1122334455667788LL)
#define FAKE_TPOFF_THREAD ((ptrdiff_t) 0xAB0)
#define FAKE_TPOFF_CACHE ((ptrdiff_t) 0xCD0)
#define FAKE_TPOFF_PLAIN ((ptrdiff_t) 0x1234)

typedef struct {
	const char *name;
	void (*emit)(CodeGenerator *generator);
	const uint8_t *expected;
	size_t expectedSize;
} GoldenCase;

static void emitLoadTlsCase(CodeGenerator *generator)
{
	asmLoadTls(&generator->buffer, TMP, FAKE_TPOFF_PLAIN);
}

static void emitCCallCase(CodeGenerator *generator)
{
	generateCCall(generator, FAKE_C_FUNCTION, 2, 0 /*storeIp*/);
}

static void emitStoreCheckCase(CodeGenerator *generator)
{
	generateStoreCheck(generator, RDI, RAX);
}

static void emitCCallPrimitiveCase(CodeGenerator *generator)
{
	generateCCallPrimitive(generator, (PrimitiveResult (*)()) FAKE_C_FUNCTION, 5);
}

static void emitEntryStubCase(CodeGenerator *generator)
{
	SmalltalkEntry.generator(generator);
}

// ---- expected vectors -------------------------------------------------------
// Captured with ST_ABI_EMIT_TEST=print from the pre-ABI-seam build (SysV).
// Regenerate with print mode whenever an emitter legitimately changes.

#include "vm/tests/EmitGoldenX64Expected.h"

static const GoldenCase Cases[] = {
	{ "asmLoadTls(TMP, 0x1234)", emitLoadTlsCase,
	  ExpectedLoadTls, sizeof(ExpectedLoadTls) },
	{ "generateCCall(fake, 2, storeIp=0)", emitCCallCase,
	  ExpectedCCall, sizeof(ExpectedCCall) },
	{ "generateStoreCheck(RDI, RAX)", emitStoreCheckCase,
	  ExpectedStoreCheck, sizeof(ExpectedStoreCheck) },
	{ "generateCCallPrimitive(fake, 5)", emitCCallPrimitiveCase,
	  ExpectedCCallPrimitive, sizeof(ExpectedCCallPrimitive) },
	{ "SmalltalkEntry stub", emitEntryStubCase,
	  ExpectedEntryStub, sizeof(ExpectedEntryStub) },
};

static void hexdumpAsCArray(const char *name, const uint8_t *bytes, size_t size)
{
	printf("static const uint8_t %s[] = {", name);
	for (size_t i = 0; i < size; i++) {
		if (i % 12 == 0) {
			printf("\n\t");
		}
		printf("0x%02X,%s", bytes[i], (i % 12 == 11 || i == size - 1) ? "" : " ");
	}
	printf("\n};\n\n");
}

static const char *CaseArrayNames[] = {
	"ExpectedLoadTls", "ExpectedCCall", "ExpectedStoreCheck",
	"ExpectedCCallPrimitive", "ExpectedEntryStub",
};

// Some sequences bake REAL C-function addresses (e.g. generateStoreCheck's
// call to rememberedSetGrow — a static-in-header copy whose address shifts
// with ASLR/relink). generateCCall emits those exclusively as
// `movabs TMP(=r10), imm64` = 49 BA <imm64>. Mask every such immediate that is
// not our deterministic FAKE_C_FUNCTION; masking is applied identically at
// capture (print) and at compare, so the vectors stay build-independent while
// the instruction STRUCTURE stays pinned.
static void maskPointerImmediates(uint8_t *bytes, size_t size)
{
	for (size_t i = 0; i + 10 <= size; i++) {
		if (bytes[i] == 0x49 && bytes[i + 1] == 0xBA) {
			uint64_t imm;
			memcpy(&imm, bytes + i + 2, 8);
			if (imm != (uint64_t) FAKE_C_FUNCTION) {
				memset(bytes + i + 2, 0xEE, 8);
			}
		}
	}
}

static size_t runCase(const GoldenCase *c, uint8_t **outBytes)
{
	CodeGenerator generator;
	memset(&generator, 0, sizeof(generator));
	asmInitBuffer(&generator.buffer, 4096);

	c->emit(&generator);

	size_t size = (size_t) asmOffset(&generator.buffer);
	*outBytes = malloc(size);
	asmCopyBuffer(&generator.buffer, *outBytes, size);
	asmFreeBuffer(&generator.buffer);
	maskPointerImmediates(*outBytes, size);
	return size;
}

// Emitters read the process-global tpoffs; pin them to fixed fakes so the
// emitted immediates are build-independent.
static void pinTlsOffsets(void)
{
	gCurrentThreadTpoff = FAKE_TPOFF_THREAD;
	gLookupCacheTpoff = FAKE_TPOFF_CACHE;
}

// Cross-member consistency of an ABI instance — what keeps a NEW instance
// honest before any code runs: every register the JIT may hold live values in
// (allocation pool + RAX result + RSI/RDI send scratch) that the C ABI
// clobbers must be in the spill list, nothing else may be, and argument
// registers must be volatile.
static int checkAbiInvariants(const X64Abi *abi)
{
	_Bool live[16] = { 0 };
	for (size_t i = 0; i < X64AvailableRegs.regsSize; i++) {
		live[X64AvailableRegs.regs[i]] = 1;
	}
	live[RAX] = live[RSI] = live[RDI] = 1;

	int errors = 0;
	for (int r = 0; r < 16; r++) {
		_Bool mustSpill = live[r] && !abi->calleeSaved[r];
		_Bool inSpill = 0;
		for (uint8_t i = 0; i < abi->callerSavedSpillCount; i++) {
			inSpill |= abi->callerSavedSpill[i] == r;
		}
		if (mustSpill != inSpill) {
			printf("ABI INVARIANT: reg %d %s the spill list\n",
				r, mustSpill ? "missing from" : "must not be in");
			errors++;
		}
	}
	for (uint8_t i = 0; i < abi->argRegsCount; i++) {
		if (abi->calleeSaved[abi->argRegs[i]]) {
			printf("ABI INVARIANT: arg register %d is callee-saved?!\n", abi->argRegs[i]);
			errors++;
		}
	}
	return errors;
}

// The CPU-model decode (jit/TargetCpu.h, jit/x64/Cpu.h) is a pure function of
// the CPUID words, so it is checkable with fabricated inputs instead of
// whatever chip happens to run the test.
//
// Two traps are guarded here. First, the raw bit values COLLIDE across leaves
// (bit 5 is AVX2 in leaf 7 EBX and LZCNT in leaf 0x80000001 ECX), so feeding a
// word from the wrong leaf silently produces a plausible wrong answer. Second,
// CPUID ALONE LIES about AVX-class features: if the OS never enabled the YMM
// state in XCR0, the instruction faults no matter what CPUID advertises.
static int checkCpuDecode(void)
{
	X64Cpu cpu;
	int errors = 0;

#define CPU_CHECK(cond, what) \
	if (!(cond)) { printf("CPU DECODE: %s\n", what); errors++; }

	// The OS-enablement trap: CPUID says AVX/FMA/AVX2, but XCR0 says the OS
	// never enabled the state. Executing them would fault, so they must decode
	// to FALSE despite being advertised.
	x64CpuDecode(&cpu, X64_CPUID1_ECX_AVX | X64_CPUID1_ECX_FMA | X64_CPUID1_ECX_OSXSAVE,
		X64_CPUID7_EBX_AVX2, 0, /* xcr0 */ 0);
	CPU_CHECK(!cpu.hasAvx, "AVX advertised but the OS never enabled YMM state");
	CPU_CHECK(!cpu.hasAvx2, "AVX2 must follow the OS's answer, not CPUID's");
	CPU_CHECK(!cpu.hasFma, "FMA is VEX-encoded: it needs the YMM state too");

	// ... and with OSXSAVE missing, XGETBV is not even legal to execute, so a
	// non-zero xcr0 must not be believed either.
	x64CpuDecode(&cpu, X64_CPUID1_ECX_AVX, 0, 0, X64_XCR0_AVX_ENABLED);
	CPU_CHECK(!cpu.hasAvx, "AVX without OSXSAVE must not be claimed");

	// The same words WITH the OS's blessing.
	x64CpuDecode(&cpu, X64_CPUID1_ECX_AVX | X64_CPUID1_ECX_FMA | X64_CPUID1_ECX_OSXSAVE,
		X64_CPUID7_EBX_AVX2, 0, X64_XCR0_AVX_ENABLED);
	CPU_CHECK(cpu.hasAvx && cpu.hasAvx2 && cpu.hasFma, "AVX-class features must decode when enabled");

	// The leaf-collision trap: bit 5 means AVX2 in leaf 7 EBX and LZCNT in the
	// extended leaf's ECX. Setting only the extended leaf must yield LZCNT and
	// NOT AVX2.
	x64CpuDecode(&cpu, 0, 0, X64_CPUIDX_ECX_LZCNT, 0);
	CPU_CHECK(cpu.hasLzcnt, "LZCNT lives in the EXTENDED leaf's ECX");
	CPU_CHECK(!cpu.hasAvx2, "the extended leaf must not be read as leaf 7");

	// GPR-only features need no OS state.
	x64CpuDecode(&cpu, 0, X64_CPUID7_EBX_BMI2, 0, 0);
	CPU_CHECK(cpu.hasBmi2, "BMI2 is GPR-only: no XCR0 involved");

	// The floor: nothing reported means nothing claimed.
	x64CpuDecode(&cpu, 0, 0, 0, 0);
	CPU_CHECK(!cpu.hasPopcnt && !cpu.hasAvx && !cpu.hasBmi2 && !cpu.hasFma,
		"an empty CPUID must claim NOTHING");

	for (const char *const *n = X64CpuNames; *n != NULL; n++) {
		if (!x64CpuByName(&cpu, *n)) {
			printf("CPU DECODE: advertised name '%s' does not decode\n", *n);
			errors++;
		}
	}
	CPU_CHECK(!x64CpuByName(&cpu, "x86-64-v42"), "an unknown name must be rejected");
#undef CPU_CHECK
	return errors;
}

int abiEmitGoldenSelfTest(const char *mode)
{
	pinTlsOffsets();

	int failures = checkAbiInvariants(gX64Abi);
	failures += checkCpuDecode();
	_Bool print = mode != NULL && strcmp(mode, "print") == 0;

	for (size_t i = 0; i < sizeof(Cases) / sizeof(Cases[0]); i++) {
		const GoldenCase *c = &Cases[i];
		uint8_t *bytes;
		size_t size = runCase(c, &bytes);

		if (print) {
			printf("// %s (%zu bytes)\n", c->name, size);
			hexdumpAsCArray(CaseArrayNames[i], bytes, size);
		} else if (size != c->expectedSize || memcmp(bytes, c->expected, size) != 0) {
			size_t firstDiff = 0;
			size_t common = size < c->expectedSize ? size : c->expectedSize;
			while (firstDiff < common && bytes[firstDiff] == c->expected[firstDiff]) {
				firstDiff++;
			}
			printf("GOLDEN MISMATCH: %s\n  expected %zu bytes, got %zu; first difference at +%zu\n",
				c->name, c->expectedSize, size, firstDiff);
			printf("  got:\n");
			hexdumpAsCArray("Actual", bytes, size);
			failures++;
		} else {
			printf("golden ok: %s (%zu bytes)\n", c->name, size);
		}
		free(bytes);
	}

	if (print) {
		return 0;
	}
	printf(failures == 0 ? "ABI emission golden test PASSED\n"
	                     : "ABI emission golden test FAILED (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
