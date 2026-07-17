// Golden-byte emission tests for the ppc64le (little-endian, ELFv2) backend:
// bring-up rung 1 of PORTING.md, and deliberately HOST-INDEPENDENT: the
// encoders emit explicitly little-endian words into a buffer, so this TU
// compiles and runs on the x86 dev host (linked into its `st` next to the x64
// and ppc64 goldens; dispatched as ST_PPC64LE_EMIT_TEST=1|print). On a real
// ppc64le build, EmitGoldenPpc64leBind.c makes this the arch's own
// ST_ABI_EMIT_TEST.
//
// The expected vectors (EmitGoldenPpc64leExpected.h) are validated against a
// CROSS objdump oracle: the same sequences assembled with
// powerpc64le-linux-gnu-as, disassembled and byte-compared. See
// scripts/ppc64/golden-oracle.sh le. ST_PPC64LE_EMIT_TEST=print regenerates
// the paste-ready arrays (all immediates are deterministic fakes, so no
// masking is needed, unlike x64).
//
// WARNING: do not include jit/CodeGenerator.h or call generic backend names
// (asmLoadTls, fiberSwitchAsm) here: in a foreign-host binary those resolve to
// the HOST backend. Everything goes through the AbiPpc64leElfV2 instance and
// the ppc64le encoders directly.
//
// Two cases exist here that the BE golden cannot have: emitCCallPrimArgs and
// emitPrimResultCheck are REAL under ELFv2 (PrimitiveResult comes back in
// r3:r4 with unshifted arguments), where ELFv1's hidden-sret convention leaves
// them FAIL()-stubbed. See vm/jit/ppc64le/DESIGN.md.
#include "vm/tests/SelfTests.h"
#include "vm/jit/ppc64le/Abi.h"
#include "vm/jit/ppc64le/Cpu.h"
#include "vm/jit/ppc64le/abi/elfv2/FiberElfV2.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define FAKE_C_FUNCTION ((intptr_t) 0x1122334455667788LL)
#define FAKE_TPOFF ((ptrdiff_t) 0x1234)
#define FAKE_TPOFF_NEG ((ptrdiff_t) -0x12344) // crosses the addis ha/lo rounding

typedef struct {
	const char *name;
	void (*emit)(AssemblerBuffer *buffer);
	const uint8_t *expected;
	size_t expectedSize;
} GoldenCase;

static void emitLi64Case(AssemblerBuffer *buffer)
{
	asmLi64(buffer, R10_PPC, (uint64_t) FAKE_C_FUNCTION);
}

// Displacements deliberately taken from the ELFv2 frame shape (TOC at 24, a
// 320-byte NV frame, FPRs at 176..312) so the case doubles as documentation.
static void emitLoadStoreCase(AssemblerBuffer *buffer)
{
	asmLd(buffer, R4, 16, R1);
	asmLd(buffer, R0, 0, R11_PPC);
	asmLd(buffer, R2, 8, R11_PPC);
	asmLd(buffer, R11_PPC, 16, R11_PPC);
	asmStd(buffer, R31, -8, R1);
	asmStd(buffer, R2, 24, R1);
	asmStdu(buffer, R1, -320, R1);
	asmLwz(buffer, R0, 8, R1);
	asmStw(buffer, R0, 8, R1);
	asmLfd(buffer, 14, 176, R1);
	asmStfd(buffer, 31, 312, R1);
}

static void emitArithCase(AssemblerBuffer *buffer)
{
	asmAddi(buffer, R5, R1, 64);
	asmAddi(buffer, R5, R1, -64);
	asmAddis(buffer, R5, R2, 0x1234);
	asmLi(buffer, R0, 0);
	asmLi(buffer, R31, -1);
	asmLis(buffer, R7, -2);
	asmMr(buffer, R3, R31);
	asmNop(buffer);
	asmSldi(buffer, R9_PPC, R9_PPC, 32);
	asmRldicr(buffer, R7, R8_PPC, 12, 50);
	asmCmpdi(buffer, 0, R3, 0);
	asmCmpdi(buffer, 7, R5, -1);
}

static void emitSprCase(AssemblerBuffer *buffer)
{
	asmMflr(buffer, R0);
	asmMtlr(buffer, R0);
	asmMtctr(buffer, R0);
	asmMfcr(buffer, R5);
	asmMtcrf(buffer, 0xFF, R5);
	asmBlr(buffer);
	asmBctr(buffer);
	asmBctrl(buffer);
}

static void emitBranchLabelCase(AssemblerBuffer *buffer)
{
	AssemblerLabel forward, forward2, backward;
	asmInitLabel(&forward);
	asmInitLabel(&forward2);
	asmInitLabel(&backward);

	asmCmpdi(buffer, 0, R3, 0);
	asmBeq(buffer, &forward);                              // forward conditional
	asmB(buffer, &forward2);                               // forward unconditional
	asmPpcLabelBind(buffer, &backward, asmOffset(buffer));
	asmNop(buffer);
	asmBne(buffer, &backward);                             // backward conditional
	asmPpcLabelBind(buffer, &forward, asmOffset(buffer));
	asmNop(buffer);
	asmPpcLabelBind(buffer, &forward2, asmOffset(buffer));
	asmB(buffer, &backward);                               // backward unconditional
}

static void emitLoadTlsCase(AssemblerBuffer *buffer)
{
	AbiPpc64leElfV2.emitLoadTls(buffer, R11_PPC, FAKE_TPOFF);
}

static void emitLoadTlsNegCase(AssemblerBuffer *buffer)
{
	AbiPpc64leElfV2.emitLoadTls(buffer, R11_PPC, FAKE_TPOFF_NEG);
}

static void emitCallCFunctionCase(AssemblerBuffer *buffer)
{
	AbiPpc64leElfV2.emitCallCFunction(buffer, FAKE_C_FUNCTION);
}

// ELFv2-only: the CCALL-primitive hooks are real here. Arity 5 mirrors the
// x64 golden's choice. The loads must read i*8(r1), NOT (i+1)*8: POWER keeps
// the return address in LR, so a frameless primitive's arg 0 sits at 0(r1).
// Same +1 x86 bias that fillVar has to undo.
static void emitCCallPrimArgsCase(AssemblerBuffer *buffer)
{
	AbiPpc64leElfV2.emitCCallPrimArgs(buffer, 5);
}

static void emitPrimResultCheckCase(AssemblerBuffer *buffer)
{
	AssemblerLabel failed;
	asmInitLabel(&failed);
	AbiPpc64leElfV2.emitPrimResultCheck(buffer, &failed);
	// Bind the forward reference so the emitted displacement is deterministic.
	asmPpcLabelBind(buffer, &failed, asmOffset(buffer));
}

static void emitEntrySaveCase(AssemblerBuffer *buffer)
{
	AbiPpc64leElfV2.emitEntrySaveRegs(buffer);
}

static void emitEntryRestoreCase(AssemblerBuffer *buffer)
{
	AbiPpc64leElfV2.emitEntryRestoreRegs(buffer);
}

static void emitSubWordCase(AssemblerBuffer *buffer)
{
	asmLbz(buffer, R5, 7, R3);
	asmStb(buffer, R5, 15, R3);
	asmLhz(buffer, R5, 6, R4);
	asmSth(buffer, R5, 6, R4);
	asmLdx(buffer, R6, R3, R5);
	asmStdx(buffer, R6, R3, R5);
	asmLbzx(buffer, R6, R3, R5);
	asmStbx(buffer, R6, R3, R5);
}

static void emitXoArithCase(AssemblerBuffer *buffer)
{
	asmAdd(buffer, R5, R3, R4);
	asmAddoDot(buffer, R5, R3, R4);
	asmSubf(buffer, R5, R4, R3);
	asmSubfoDot(buffer, R5, R4, R3);
	asmNeg(buffer, R5, R3);
	asmNegoDot(buffer, R5, R3);
	asmMulld(buffer, R5, R3, R4);
	asmMulldoDot(buffer, R5, R3, R4);
	asmDivd(buffer, R5, R3, R4);
	asmAnd(buffer, R5, R3, R4);
	asmXor(buffer, R5, R3, R4);
	asmAndiDot(buffer, R0, R3, 3);
	asmAddicDot(buffer, R14_PPC, R14_PPC, -1);
}

static void emitShiftCmpCase(AssemblerBuffer *buffer)
{
	asmSrdi(buffer, R5, R3, 4);
	asmClrldi(buffer, R5, R3, 56);
	asmSradi(buffer, R5, R3, 2);
	asmSld(buffer, R5, R3, R4);
	asmSrd(buffer, R5, R3, R4);
	asmSrad(buffer, R5, R3, R4);
	asmCmpd(buffer, 0, R3, R4);
	asmCmpld(buffer, 7, R3, R4);
	asmCmpldi(buffer, 1, R3, 4095);
}

static void emitFloatXerCase(AssemblerBuffer *buffer)
{
	asmFadd(buffer, 0, 0, 1);
	asmFsub(buffer, 0, 0, 1);
	asmFmul(buffer, 0, 0, 1);
	asmFdiv(buffer, 0, 0, 1);
	asmFcmpu(buffer, 1, 0, 1);
	asmClearXerSo(buffer);
	asmBclNext(buffer);
	asmPush(buffer, R3);
	asmPop(buffer, R3);
	asmCallReg(buffer, R12_PPC);
}


// The SmallFloat64 decode/encode building blocks: pure 64-bit rotates plus
// the ISA 2.07 GPR<->VSR moves (emitted only behind the isPower8 gate).
static void emitSmallFloatOpsCase(AssemblerBuffer *buffer)
{
	asmRotldi(buffer, R5, R3, 1);
	asmRotrdi(buffer, R5, R3, 1);
	asmRotldi(buffer, R0, R4, 63);
	asmMtvsrd(buffer, 0, R5);
	asmMtvsrd(buffer, 31, TMP2);
	asmMfvsrd(buffer, R5, 0);
	asmMfvsrd(buffer, TMP2, 31);
}

// ---- expected vectors -------------------------------------------------------
// Validated against powerpc64le-linux-gnu-as + objdump (the cross oracle):
// scripts/ppc64/golden-oracle.sh le. Regenerate with ST_PPC64LE_EMIT_TEST=print.

#include "vm/tests/EmitGoldenPpc64leExpected.h"

static const GoldenCase Cases[] = {
	{ "asmLi64(r10, 0x1122334455667788)", emitLi64Case,
	  ExpectedPpcLeLi64, sizeof(ExpectedPpcLeLi64) },
	{ "ld/std/stdu/lwz/stw/lfd/stfd", emitLoadStoreCase,
	  ExpectedPpcLeLoadStore, sizeof(ExpectedPpcLeLoadStore) },
	{ "addi/addis/li/lis/mr/rldicr/cmpdi", emitArithCase,
	  ExpectedPpcLeArith, sizeof(ExpectedPpcLeArith) },
	{ "mflr/mtlr/mtctr/mfcr/mtcrf/blr/bctr/bctrl", emitSprCase,
	  ExpectedPpcLeSpr, sizeof(ExpectedPpcLeSpr) },
	{ "b/bc labels (forward + backward)", emitBranchLabelCase,
	  ExpectedPpcLeBranch, sizeof(ExpectedPpcLeBranch) },
	{ "elfv2 emitLoadTls(r11, 0x1234)", emitLoadTlsCase,
	  ExpectedPpcLeLoadTls, sizeof(ExpectedPpcLeLoadTls) },
	{ "elfv2 emitLoadTls(r11, -0x12344)", emitLoadTlsNegCase,
	  ExpectedPpcLeLoadTlsNeg, sizeof(ExpectedPpcLeLoadTlsNeg) },
	{ "elfv2 emitCallCFunction (r12, no descriptor)", emitCallCFunctionCase,
	  ExpectedPpcLeCallCFunction, sizeof(ExpectedPpcLeCallCFunction) },
	{ "elfv2 emitCCallPrimArgs(5)", emitCCallPrimArgsCase,
	  ExpectedPpcLeCCallPrimArgs, sizeof(ExpectedPpcLeCCallPrimArgs) },
	{ "elfv2 emitPrimResultCheck (r3:r4)", emitPrimResultCheckCase,
	  ExpectedPpcLePrimResultCheck, sizeof(ExpectedPpcLePrimResultCheck) },
	{ "elfv2 entry save regs", emitEntrySaveCase,
	  ExpectedPpcLeEntrySave, sizeof(ExpectedPpcLeEntrySave) },
	{ "elfv2 entry restore regs", emitEntryRestoreCase,
	  ExpectedPpcLeEntryRestore, sizeof(ExpectedPpcLeEntryRestore) },
	{ "lbz/stb/lhz/sth/ldx/stdx/lbzx/stbx", emitSubWordCase,
	  ExpectedPpcLeSubWord, sizeof(ExpectedPpcLeSubWord) },
	{ "add/subf/neg/mulld/divd + OE.Rc/andi./addic.", emitXoArithCase,
	  ExpectedPpcLeXoArith, sizeof(ExpectedPpcLeXoArith) },
	{ "srdi/clrldi/sradi/sld/srd/srad/cmpd/cmpld/cmpldi", emitShiftCmpCase,
	  ExpectedPpcLeShiftCmp, sizeof(ExpectedPpcLeShiftCmp) },
	{ "fadd/fsub/fmul/fdiv/fcmpu/xer/bcl/push/pop/callreg", emitFloatXerCase,
	  ExpectedPpcLeFloatXer, sizeof(ExpectedPpcLeFloatXer) },
	{ "smallfloat rotldi/rotrdi/mtvsrd/mfvsrd", emitSmallFloatOpsCase,
	  ExpectedPpcLeSmallFloatOps, sizeof(ExpectedPpcLeSmallFloatOps) },
};

static const char *CaseArrayNames[] = {
	"ExpectedPpcLeLi64", "ExpectedPpcLeLoadStore", "ExpectedPpcLeArith",
	"ExpectedPpcLeSpr", "ExpectedPpcLeBranch", "ExpectedPpcLeLoadTls",
	"ExpectedPpcLeLoadTlsNeg", "ExpectedPpcLeCallCFunction",
	"ExpectedPpcLeCCallPrimArgs", "ExpectedPpcLePrimResultCheck",
	"ExpectedPpcLeEntrySave", "ExpectedPpcLeEntryRestore",
	"ExpectedPpcLeSubWord", "ExpectedPpcLeXoArith", "ExpectedPpcLeShiftCmp",
	"ExpectedPpcLeFloatXer", "ExpectedPpcLeSmallFloatOps",
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

static size_t runCase(const GoldenCase *c, uint8_t **outBytes)
{
	AssemblerBuffer buffer;
	asmInitBuffer(&buffer, 4096);
	c->emit(&buffer);

	size_t size = (size_t) asmOffset(&buffer);
	*outBytes = malloc(size);
	asmCopyBuffer(&buffer, *outBytes, size);
	asmFreeBuffer(&buffer);
	return size;
}

// asmLi64Patch must turn an emitted li64(A) into a byte-exact li64(B) by
// rewriting only the four immediate halves: the mechanism a GC pointer-patch
// loop relies on. Self-checking (no pinned vector). This is THE highest-risk
// hand translation of the LE port, since every byte index moves relative to
// the BE version, so it runs before any pinned case.
static int checkLi64Patch(void)
{
	AssemblerBuffer a, b;
	asmInitBuffer(&a, 64);
	asmInitBuffer(&b, 64);
	asmLi64(&a, R16, 0x0123456789ABCDEFULL);
	asmLi64(&b, R16, 0xFEDCBA9876543210ULL);
	asmLi64Patch(a.buffer, 0xFEDCBA9876543210ULL);

	int errors = 0;
	if (asmOffset(&a) != 20 || asmOffset(&b) != 20
			|| memcmp(a.buffer, b.buffer, 20) != 0) {
		printf("LI64 PATCH: patched sequence differs from directly-emitted one\n");
		errors = 1;
	}
	if (asmLi64Read(a.buffer) != 0xFEDCBA9876543210ULL) {
		printf("LI64 READ: decoded immediate differs from what was patched in\n");
		errors++;
	}
	asmFreeBuffer(&a);
	asmFreeBuffer(&b);
	return errors;
}

// The prime half of the fiber pair is plain C: verify the exact frame layout
// the fiberSwitchElfV2 restore path consumes, natively.
//
// Two ELFv2 deltas from the BE check: `entry` is the raw code address (there
// is no descriptor to dereference), and the r2 slot must stay ZERO, because
// the switch delivers r12 = entry (its `mr 12,0`) and the entry's global-entry
// prologue derives its own TOC from that. A non-zero r2 slot here would mean
// someone re-introduced ELFv1 thinking.
static int checkFiberPrimeLayout(void)
{
	static uint8_t stack[4096];
	void (*fakeEntry)(void) = (void (*)(void)) (uintptr_t) 0xAAAAAAAA11111111ULL;
	uint8_t *top = stack + sizeof(stack);
	void *sp = AbiPpc64leElfV2.fiberPrimeStack(top, fakeEntry);

	uintptr_t spValue = (uintptr_t) sp;
	uintptr_t base = ((uintptr_t) top - 64) & ~(uintptr_t) 15;
	int errors = 0;

#define PRIME_CHECK(cond, what) \
	if (!(cond)) { printf("FIBER PRIME: %s\n", what); errors++; }

	PRIME_CHECK(spValue % 16 == 0, "sp not 16-byte aligned");
	PRIME_CHECK(spValue == base - ELFV2_NV_FRAME_SIZE, "sp not one NV frame below base");
	PRIME_CHECK(*(uintptr_t *) spValue == base, "back chain does not point at base");
	PRIME_CHECK(*(uintptr_t *) (spValue + ELFV2_NV_FRAME_TOC) == 0,
		"r2 slot seeded (ELFv2 derives r2 from r12, it must stay zero)");
	PRIME_CHECK(*(uintptr_t *) (base + ELFV2_HEADER_LR_SAVE) == (uintptr_t) fakeEntry,
		"LR slot != entry address");
	PRIME_CHECK(*(uintptr_t *) (base + ELFV2_HEADER_CR_SAVE) == 0, "CR slot not zero");
	PRIME_CHECK(*(uintptr_t *) base == 0, "terminator back chain not null");
	for (int i = 0; i < 18; i++) {
		PRIME_CHECK(*(uintptr_t *) (spValue + ELFV2_NV_FRAME_GPRS + i * 8) == 0,
			"GPR slot not zero");
		PRIME_CHECK(*(uintptr_t *) (spValue + ELFV2_NV_FRAME_FPRS + i * 8) == 0,
			"FPR slot not zero");
	}
#undef PRIME_CHECK
	return errors;
}

// Cross-member consistency of the ABI instance (mirrors the x64 and ppc64
// goldens): every register the JIT may hold live values in (allocation pool
// plus the r3-r8 result/dispatch-scratch extras, see the role table in
// AssemblerPpc64le.h) that the C ABI clobbers must be in the spill list,
// nothing else may be, and argument registers must be volatile. This is what
// keeps the instance honest whenever the pool is revisited.
static int checkAbiInvariants(const Ppc64leAbi *abi)
{
	_Bool live[32] = { 0 };
	for (size_t i = 0; i < Ppc64AvailableRegs.regsSize; i++) {
		live[Ppc64AvailableRegs.regs[i]] = 1;
	}
	live[R3] = live[R4] = live[R5] = live[R6] = live[R7] = live[R8_PPC] = 1;

	int errors = 0;
	for (int r = 0; r < 32; r++) {
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

// The CPU-model decode (jit/TargetCpu.h, jit/ppc64le/Cpu.h) is a pure function
// of the kernel's hwcap words, so it is checkable HERE, natively, with
// fabricated inputs: "read the wrong bit" is invisible end-to-end, since a wrong
// feature flag silently loses an optimization or wrongly enables an instruction
// the CPU does not have.
//
// Deliberately shorter than the BE backend's: ppc64le has no pre-POWER8 member,
// so there is no AltiVec-without-VSX or POWER5-without-AltiVec case to guard.
static int checkCpuDecode(void)
{
	Ppc64leCpu cpu;
	int errors = 0;

#define CPU_CHECK(cond, what) \
	if (!(cond)) { printf("CPU DECODE: %s\n", what); errors++; }

	ppc64leCpuByName(&cpu, "power8");
	CPU_CHECK(cpu.hasVsx && cpu.hasAltivec, "the ppc64le floor includes VSX and AltiVec");
	CPU_CHECK(cpu.hasGprVsrMoves, "power8 must have the ISA 2.07 GPR<->VSR moves");
	CPU_CHECK(!cpu.isPower9 && !cpu.isPower10, "power8 level wrong");
	CPU_CHECK(strcmp(cpu.name, "power8") == 0, "power8 misnamed");

	ppc64leCpuByName(&cpu, "power9");
	CPU_CHECK(cpu.isPower9 && !cpu.isPower10, "power9 level wrong");

	// Levels must be CUMULATIVE downward, whatever a stingy reporter says.
	ppc64leCpuByName(&cpu, "power10");
	CPU_CHECK(cpu.isPower10 && cpu.isPower9, "power10 must imply power9");

	// The derived capability needs BOTH halves, exactly like the BE decode:
	// ISA 2.07 says mtvsrd/mfvsrd exist, the VSX facility bit says the OS
	// enabled the register state. A kernel with VSX disabled must not claim
	// the moves despite the ISA level.
	ppc64leCpuDecode(&cpu, PPC64LE_FEATURE_64, PPC64LE_FEATURE2_ARCH_2_07);
	CPU_CHECK(!cpu.hasVsx && !cpu.hasGprVsrMoves,
		"ISA 2.07 without the VSX facility must NOT claim the GPR<->VSR moves");

	// An under-reporting host must claim NOTHING rather than inherit the floor:
	// the global starts at the baseline, but a decode is only ever what it read.
	ppc64leCpuDecode(&cpu, 0, 0);
	CPU_CHECK(!cpu.isPower9 && !cpu.isPower10 && !cpu.hasVsx && !cpu.hasAltivec
			&& !cpu.hasGprVsrMoves,
		"an empty hwcap must claim NOTHING");

	// The default global, by contrast, IS the architecture's floor.
	CPU_CHECK(gPpc64leCpu.hasVsx && gPpc64leCpu.hasAltivec && gPpc64leCpu.hasGprVsrMoves,
		"the ppc64le baseline global must assume POWER8");

	for (const char *const *n = Ppc64leCpuNames; *n != NULL; n++) {
		if (!ppc64leCpuByName(&cpu, *n)) {
			printf("CPU DECODE: advertised name '%s' does not decode\n", *n);
			errors++;
		}
	}
	CPU_CHECK(!ppc64leCpuByName(&cpu, "power42"), "an unknown name must be rejected");
#undef CPU_CHECK
	return errors;
}

int ppc64leEmitGoldenSelfTest(const char *mode)
{
	int failures = checkAbiInvariants(&AbiPpc64leElfV2);
	failures += checkLi64Patch();
	failures += checkFiberPrimeLayout();
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
	printf(failures == 0 ? "ppc64le emission golden test PASSED\n"
	                     : "ppc64le emission golden test FAILED (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
