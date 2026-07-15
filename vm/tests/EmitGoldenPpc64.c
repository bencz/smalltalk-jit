// Golden-byte emission tests for the ppc64 (big-endian, ELFv1) backend —
// bring-up rung 1 of PORTING.md, and deliberately HOST-INDEPENDENT: the
// encoders emit explicitly big-endian words into a buffer, so this TU
// compiles and runs on the x86 dev host (linked into its `st` next to the
// x64 golden; dispatched as ST_PPC64_EMIT_TEST=1|print). On a real ppc64
// build, EmitGoldenPpc64Bind.c makes this the arch's own ST_ABI_EMIT_TEST.
//
// The expected vectors (EmitGoldenPpc64Expected.h) are validated against a
// CROSS objdump oracle: the same sequences assembled with
// powerpc64-linux-gnu-as, disassembled, and byte-compared — see
// scripts/ppc64/golden-oracle.sh. ST_PPC64_EMIT_TEST=print regenerates the
// paste-ready arrays (all immediates are deterministic fakes; no masking
// needed, unlike x64).
//
// ⚠ Do not include jit/CodeGenerator.h or call generic backend names
// (asmLoadTls, fiberSwitchAsm) here: in a foreign-host binary those resolve
// to the HOST backend. Everything goes through the AbiPpc64ElfV1 instance
// and the ppc64 encoders directly.
#include "vm/tests/SelfTests.h"
#include "vm/jit/ppc64/Abi.h"
#include "vm/jit/ppc64/abi/elfv1/FiberElfV1.h"
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

static void emitLoadStoreCase(AssemblerBuffer *buffer)
{
	asmLd(buffer, R4, 16, R1);
	asmLd(buffer, R0, 0, R11_PPC);
	asmLd(buffer, R2, 8, R11_PPC);
	asmLd(buffer, R11_PPC, 16, R11_PPC);
	asmStd(buffer, R31, -8, R1);
	asmStd(buffer, R2, 40, R1);
	asmStdu(buffer, R1, -336, R1);
	asmLwz(buffer, R0, 8, R1);
	asmStw(buffer, R0, 8, R1);
	asmLfd(buffer, 14, 192, R1);
	asmStfd(buffer, 31, 328, R1);
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
	AbiPpc64ElfV1.emitLoadTls(buffer, R11_PPC, FAKE_TPOFF);
}

static void emitLoadTlsNegCase(AssemblerBuffer *buffer)
{
	AbiPpc64ElfV1.emitLoadTls(buffer, R11_PPC, FAKE_TPOFF_NEG);
}

static void emitCallCFunctionCase(AssemblerBuffer *buffer)
{
	AbiPpc64ElfV1.emitCallCFunction(buffer, FAKE_C_FUNCTION);
}

static void emitEntrySaveCase(AssemblerBuffer *buffer)
{
	AbiPpc64ElfV1.emitEntrySaveRegs(buffer);
}

static void emitEntryRestoreCase(AssemblerBuffer *buffer)
{
	AbiPpc64ElfV1.emitEntryRestoreRegs(buffer);
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

// ---- expected vectors -------------------------------------------------------
// Validated against powerpc64-linux-gnu-as + objdump (the cross oracle) —
// scripts/ppc64/golden-oracle.sh. Regenerate with ST_PPC64_EMIT_TEST=print.

#include "vm/tests/EmitGoldenPpc64Expected.h"

static const GoldenCase Cases[] = {
	{ "asmLi64(r10, 0x1122334455667788)", emitLi64Case,
	  ExpectedPpcLi64, sizeof(ExpectedPpcLi64) },
	{ "ld/std/stdu/lwz/stw/lfd/stfd", emitLoadStoreCase,
	  ExpectedPpcLoadStore, sizeof(ExpectedPpcLoadStore) },
	{ "addi/addis/li/lis/mr/rldicr/cmpdi", emitArithCase,
	  ExpectedPpcArith, sizeof(ExpectedPpcArith) },
	{ "mflr/mtlr/mtctr/mfcr/mtcrf/blr/bctr/bctrl", emitSprCase,
	  ExpectedPpcSpr, sizeof(ExpectedPpcSpr) },
	{ "b/bc labels (forward + backward)", emitBranchLabelCase,
	  ExpectedPpcBranch, sizeof(ExpectedPpcBranch) },
	{ "elfv1 emitLoadTls(r11, 0x1234)", emitLoadTlsCase,
	  ExpectedPpcLoadTls, sizeof(ExpectedPpcLoadTls) },
	{ "elfv1 emitLoadTls(r11, -0x12344)", emitLoadTlsNegCase,
	  ExpectedPpcLoadTlsNeg, sizeof(ExpectedPpcLoadTlsNeg) },
	{ "elfv1 emitCallCFunction (descriptor)", emitCallCFunctionCase,
	  ExpectedPpcCallCFunction, sizeof(ExpectedPpcCallCFunction) },
	{ "elfv1 entry save regs", emitEntrySaveCase,
	  ExpectedPpcEntrySave, sizeof(ExpectedPpcEntrySave) },
	{ "elfv1 entry restore regs", emitEntryRestoreCase,
	  ExpectedPpcEntryRestore, sizeof(ExpectedPpcEntryRestore) },
	{ "lbz/stb/lhz/sth/ldx/stdx/lbzx/stbx", emitSubWordCase,
	  ExpectedPpcSubWord, sizeof(ExpectedPpcSubWord) },
	{ "add/subf/neg/mulld/divd + OE.Rc/andi./addic.", emitXoArithCase,
	  ExpectedPpcXoArith, sizeof(ExpectedPpcXoArith) },
	{ "srdi/clrldi/sradi/sld/srd/srad/cmpd/cmpld/cmpldi", emitShiftCmpCase,
	  ExpectedPpcShiftCmp, sizeof(ExpectedPpcShiftCmp) },
	{ "fadd/fsub/fmul/fdiv/fcmpu/xer/bcl/push/pop/callreg", emitFloatXerCase,
	  ExpectedPpcFloatXer, sizeof(ExpectedPpcFloatXer) },
};

static const char *CaseArrayNames[] = {
	"ExpectedPpcLi64", "ExpectedPpcLoadStore", "ExpectedPpcArith",
	"ExpectedPpcSpr", "ExpectedPpcBranch", "ExpectedPpcLoadTls",
	"ExpectedPpcLoadTlsNeg", "ExpectedPpcCallCFunction",
	"ExpectedPpcEntrySave", "ExpectedPpcEntryRestore",
	"ExpectedPpcSubWord", "ExpectedPpcXoArith", "ExpectedPpcShiftCmp",
	"ExpectedPpcFloatXer",
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

// asmLi64Patch must turn an emitted li64(A) into byte-exact li64(B) by
// rewriting only the four immediate halves — the mechanism a GC
// pointer-patch loop will rely on. Self-checking (no pinned vector).
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

// The prime half of the fiber pair is plain C — verify the exact frame
// layout the fiberSwitchElfV1 restore path consumes, natively. Under ELFv1
// the entry function pointer is a DESCRIPTOR pointer: prime must plant the
// descriptor's entry in the LR slot and its TOC in the r2 slot.
static int checkFiberPrimeLayout(void)
{
	static uint8_t stack[4096];
	uintptr_t fakeDescriptor[3] = {
		(uintptr_t) 0xAAAAAAAA11111111ULL,
		(uintptr_t) 0xBBBBBBBB22222222ULL,
		0,
	};
	uint8_t *top = stack + sizeof(stack);
	void *sp = AbiPpc64ElfV1.fiberPrimeStack(top,
		(void (*)(void)) (uintptr_t) fakeDescriptor);

	uintptr_t spValue = (uintptr_t) sp;
	uintptr_t base = ((uintptr_t) top - 64) & ~(uintptr_t) 15;
	int errors = 0;

#define PRIME_CHECK(cond, what) \
	if (!(cond)) { printf("FIBER PRIME: %s\n", what); errors++; }

	PRIME_CHECK(spValue % 16 == 0, "sp not 16-byte aligned");
	PRIME_CHECK(spValue == base - ELFV1_NV_FRAME_SIZE, "sp not one NV frame below base");
	PRIME_CHECK(*(uintptr_t *) spValue == base, "back chain does not point at base");
	PRIME_CHECK(*(uintptr_t *) (spValue + ELFV1_NV_FRAME_TOC) == fakeDescriptor[1],
		"r2 slot != descriptor TOC");
	PRIME_CHECK(*(uintptr_t *) (base + ELFV1_HEADER_LR_SAVE) == fakeDescriptor[0],
		"LR slot != descriptor entry");
	PRIME_CHECK(*(uintptr_t *) (base + ELFV1_HEADER_CR_SAVE) == 0, "CR slot not zero");
	PRIME_CHECK(*(uintptr_t *) base == 0, "terminator back chain not null");
	for (int i = 0; i < 18; i++) {
		PRIME_CHECK(*(uintptr_t *) (spValue + ELFV1_NV_FRAME_GPRS + i * 8) == 0,
			"GPR slot not zero");
		PRIME_CHECK(*(uintptr_t *) (spValue + ELFV1_NV_FRAME_FPRS + i * 8) == 0,
			"FPR slot not zero");
	}
#undef PRIME_CHECK
	return errors;
}

// Cross-member consistency of the ABI instance (mirrors the x64 golden):
// every register the JIT may hold live values in (allocation pool + the
// r3-r8 result/dispatch-scratch extras — see the role table in
// AssemblerPpc64.h) that the C ABI clobbers must be in the spill list,
// nothing else may be, and argument registers must be volatile. This is what
// keeps the instance honest whenever the pool is revisited.
static int checkAbiInvariants(const Ppc64Abi *abi)
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

int ppc64EmitGoldenSelfTest(const char *mode)
{
	int failures = checkAbiInvariants(&AbiPpc64ElfV1);
	failures += checkLi64Patch();
	failures += checkFiberPrimeLayout();
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
	printf(failures == 0 ? "ppc64 emission golden test PASSED\n"
	                     : "ppc64 emission golden test FAILED (%d)\n", failures);
	return failures == 0 ? 0 : 1;
}
