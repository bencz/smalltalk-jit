# ppc64 (BE/ELFv1) backend design — the x64→POWER mapping

Goal: `build.sh` (cmake + full `run_tests.sh --all`) green natively on ppc64,
then ppc64le. The x64 backend is the SPEC; this file pins every mapping
decision so the translation is mechanical and reviewable. Read together with
PORTING.md (bring-up ladder, ELFv1 facts) and the golden harness
(`ST_PPC64_EMIT_TEST`, scripts/ppc64/golden-oracle.sh).

## Register roles (VM-internal convention)

| role | x64 | ppc64 | notes |
|---|---|---|---|
| SP (Smalltalk stack = C stack) | RSP | r1 | push = `stdu src,-8(r1)` (atomic dec+store, signal-safe); pop = `ld dst,0(r1); addi r1,r1,8` |
| FP (StackFrame*) | RBP | **r31** | nonvolatile; frame layout byte-identical to x64 (walker in StackFrame.c untouched) |
| CTX (context) | R12 | **r30** | nonvolatile |
| send target (native-code entry) | R11 | **r12** | volatile; call = `mtctr r12; bctrl`; ELFv2-friendly later |
| TMP (per-instruction scratch) | R10 | **r11** | volatile; also the ELFv1 descriptor/env reg in emitCallCFunction (by ABI role, never live across) |
| result | RAX | **r3** | = C result reg, same trick as x64 |
| dispatch scratch A (class / C arg0) | RDI | **r3** | ⚠ RDI and RAX BOTH map to r3 — resolve per site; the C-arg pre-placement trick survives: lookupNativeCode(class=r3, selector=r4), allocateObject(heap=r3, class=r4, size=r5) |
| dispatch scratch B (selector / C arg1) | RSI | **r4** | |
| dispatch scratch C (size/hash / C arg2) | RDX | **r5** | |
| extra scratch | RCX, R8, R9 | **r6, r7, r8** | |
| callee-saved scratch (entry-stub arg counter) | RBX | **r14** | inside the entry hook's saved area |
| prim scratch preserved across CCall | R13 | **r15** | nonvolatile (generateCCallPrimitive R11↔R13 dance → r12↔r15) |
| FP scratch | XMM0/XMM1 | **f0/f1** | volatile; fadd/fsub/fmul/fdiv/fcmpu |
| allocation pool | {R11,R13,R14,R15,RBX,R9,R8,RCX,RDX} | **{r16..r24} ∪ {r9,r10}** | nonvolatile-heavy; NEVER r0/r1/r2/r13(TP)/r30/r31/r12/r11 and not r3-r8 (dispatch scratch). Spill list = {r3..r10} (volatile ∩ (pool ∪ extras r3-r8)) — keep the golden invariant + Abi tables in sync when changing |

r0 quirk: reads as literal 0 in D-form RA slots — used ONLY as throwaway
(andi. target, LR/CR staging), never allocated.

## Frames (must match StackFrame.c exactly — PORT_ME(frame-layout))

Caller pushes args (stdu each), callee entered via `mtctr r12; bctrl`
(return addr in LR, NOT on the stack — the one structural difference).

Method/framed prologue reproduces the x64 picture explicitly:
```
mflr r0
stdu r0, -8(r1)        ; parentIc slot   (x64: pushed by `call`)
stdu r31, -8(r1)       ; saved FP        (x64: push rbp)
mr   r31, r1           ; FP = StackFrame*(= &saved-FP)
addi r1, r1, -frameSize*8  ; then nil-fill slots (same as x64)
std  r12, -8(r31)      ; slot 0 = native-code insts (from send target reg)
```
Layout identical to x64: args at FP+16+, parentIc at FP+8, saved FP at FP+0,
slot k at FP-8*(k+1). Epilogue: `addi r1,r1,frameSize*8; ld r31,0(r1);
ld r0,8(r1); mtlr r0; addi r1,r1,16; blr`.

**LR discipline**: LR is dead inside method bodies after the prologue (return
addr lives in the frame). Rules: (1) every FRAMED primitive (x64 "push rbp"
prims) starts with the `mflr r0; stdu r0,-8(r1)` push, making all x64 frame
offsets identical; (2) generateCCall itself saves/restores LR around the C
call (x64 didn't need to — `call` used memory); (3) frameless prims (IntAdd
etc.) never emit calls on the fast path and fall through with LR intact;
their arg(i) = `i*8(r1)` (NO +8 — no pushed return addr!). Framed-prim arg
offsets via FP stay x64-identical (FP+16 = receiver...).

## BRING-UP BUGS root-caused under qemu (all fixed — the war stories)

1. **GC frame walk needs the return address ON THE STACK at [tempFP+8]** —
   x64's `call` provides it implicitly; the first generateCCall stashed LR
   at 88(r1) instead and every scavenge inside a C call mis-resolved frame
   stackmaps → stale-pointer corruption (a String's size field ate a rem-set
   append). generateCCall now PUSHES LR before FP, byte-mirroring x64.
2. **Frameless arg offsets are +1-biased** by the shared RegisterAllocator
   (x64's call-pushed return address). POWER keeps it in LR: a frameless
   setter read `self` from slot 1 = its ARGUMENT. fillVar un-biases when
   frameLess.
3. **`RegisterAllocator.c` pinned the context to hardcoded register 12**
   (x64's R12=CTX; ppc64's r12=TGT — clobbered at every send). Now `CTX`.
4. **SPILLED_REG (-1) leaks into emitters**: x64 encodes -1&7 = RDI and
   WORKS BY ACCIDENT (a de-facto per-copy scratch); POWER encoded -1&31 =
   r31 = FP and died. ppc64 makes the accident explicit: `varReg()` maps
   SPILLED_REG → r4 at every emitter-feeding site, and the D/DS/X-form
   builders ASSERT against negative registers.
5. **CompiledCodeHeader-as-Value is endian-sensitive** (it sits in the
   scanned vars area of CompiledMethod/Block): on LE the word's low byte is
   `tag`(=0) → always reads as SmallInteger; on BE it was `primitive`'s low
   byte → odd primitive numbers looked like heap POINTERS to the GC and the
   snapshot walker. The struct's field order is mirrored under
   TARGET_BIG_ENDIAN (core/CompiledCode.h).
6. **qemu-user delivers si_addr=NULL (and DAR=0)** on guard faults → the
   fiber grow-on-fault handler cannot attribute them. Workarounds: the
   SEGV_ACCERR si_code filter removed (the callback validates addresses
   itself) and `ST_FIBER_PRECOMMIT=1` commits fiber stacks fully at creation
   for qemu-user runs (real kernels keep the lazy default).
7. **gdb-under-qemu gotchas**: addresses shift with/without `-g` (use
   in-process marker functions, not addresses from other runs); hardware
   watchpoints unsupported (software `watch` works: single-steps); a failed
   cross-compile leaves the OLD binary in place — always check for
   "build-nocmake done".

## REFINEMENTS locked during implementation (supersede anything below)

- **TMP2 = r10**: second dedicated scratch, owned by asmLdT/asmStdT — DS-form
  (ld/std) cannot encode tag-folded displacements (offsetof-1 ≡ 3 mod 4), so
  those helpers untag the base into TMP2 first. Pool shrank to
  {r16..r24, r9}; spill list = {r3..r8, r9} = {r3..r9} minus nothing — i.e.
  {R3,R4,R5,R6,R7,R8,R9}; golden extras = r3..r8. lwz/lbz/lhz/lfd (D-form)
  take ANY displacement — sub-word and float accesses need no helper.
- **r0 rules**: OK as computation target and X-form operand; NEVER a D/DS
  base (reads as literal 0); addi/addis with RA=r0 read literal 0 (so word
  RMW uses TMP2: ld TMP2/addi TMP2/std TMP2; byte RMW may use r0 with
  ori/stb).
- **generateCCall LR**: LR is stashed in the ELFv1 frame's param-save area at
  **88(r1)** (NOT pushed!) so the temp-frame shape stays x64-identical —
  [FP+8] must hold the storeIp IP (walker parentIc), and frameless setters'
  store-check grow path keeps LR alive through the C call. Sequence:
  [storeIp: bcl 20,31,$+4; generateStackmap(); mflr TMP; push TMP];
  push FP; mr FP,r1; push CTX; clrrdi r1,4; stdu r1,-112(r1);
  mflr r0; std r0,88(r1); TLS exit-frame store; emitCallCFunction;
  ld r0,88(r1); mtlr r0; ld CTX,-8(FP); mr r1,FP; pop FP; [storeIp: drop 8].
  Note: NO LR slot in the pushed area — x64 layout byte-compatible.
- **generateCCallPrimitive (ELFv1, FUSED — does not reuse generateCCall)**:
  prim entry (frameless, arg i at i*8(r1), LR = send-site return):
  mflr r0; push r0   ← this LR push IS the temp frame's parentIc ([FP+8] =
  send-site IC, exactly x64's call-pushed retaddr); push FP; mr FP,r1;
  push CTX; mr r15,TGT (the x64 R11→R13 dance); clrrdi r1,4;
  stdu r1,-112(r1); TLS exit-frame store; addi r3,r1,96 (hidden sret);
  Smalltalk arg i (now at (i+2)*8(FP)... arg0 at FP+16) → r4..r8;
  emitCallCFunction; ld r3,96(r1) value; ld r4,104(r1) failed (BEFORE
  teardown — below-r1 memory is signal-clobberable);
  ld CTX,-8(FP); mr r1,FP; pop FP; pop-LR (ld r0,0(r1); mtlr; +8);
  cmpdi r4,0; bne failed; blr; failed: mr TGT,r15; fall through.
- **AllocateStub register budget**: class=r4, indexedSize=r5, instSize=r6
  (x64 RCX), varsSize=r7 (x64 RDI), thread base=r9 (x64 RBX) until the TLAB
  bump/noFreeSpace branch, THEN r9 becomes the loop counter; r8 = untagged
  body cursor base; body-pointer (x64 RCX repurpose) = r6 after instSize is
  dead; result r3 (= old TLAB top). Header init (BE!): stw hash,8(r3) + stw
  zero,12(r3). Slow path: ld r3,Thread.heap(r9) → allocateObject(r3,r4,r5)
  args pre-placed.
- **generateMethodLookup cache probe**: untag class (addi r3,-1); hash: xor
  r5,r3,r4; srdi 4; andi. 4095; sldi r0,r5,3; add r6,TMP,r0 (TMP = TLS cache
  base); ld r0,0(r6) = classes[i]; addis r6,r6,1 (+65536); selectors[i] =
  ld -32768(r6); codes[i] = ld 0(r6) → TGT. (Array strides 32768/65536
  exceed D-form range — the addis trick avoids ldx re-scaling.)
- **Float fast path scratch**: result/new-Float in r3 forces the reloads to
  r4 (receiver) and r6 (arg — NOT r3/RDI as on x64); lfd/stfd take the
  tag-folded disp 15 directly (D-form).
- **generateBlockContextAllocation**: block pointer lives in r6 (x64 used
  RDI while RAX held the context — r3 collision), and BlockValue prims read
  the block from r6 after it returns.
- **Entry stub**: counter r14; arg loop: sldi r0,r14,3; add r9,rArgs,r0;
  ld r9,-8(r9); push r9; addic. r14,-1; bne. r12=TGT gets the entry before
  bctrl (VM convention).
- **Frameless prims + bare C calls**: x64 sometimes bare-calls C
  (StringHash/exit/notImplemented) — on ppc64 ALL C calls go through
  generateCCall (descriptor discipline); its LR stash keeps frameless blr
  valid. asmInt3 → `trap` (0x7FE00008, asmTrap).
- **Misses re-arm XER[SO]** (asmClearXerSo) at the tagMiss/overflowMiss join
  in generateSend's int path before falling into float/dispatch.
- **cmp operand orders verified**: asmCmpq(A,B)+cond ⇔ A cond B;
  asmCmpqMem(mem,reg) = reg − [mem]; asmAddbMem(mem,reg): reg += [mem];
  asmAddqToMem(reg,mem): [mem] += reg; asmAddqMemImm(mem,imm): [mem] += imm;
  asmImulqMem(mem,dst): dst *= [mem]; asmOrbMemImm: [mem] |= imm.

## C calls (generateCCall, ELFv1)

```
[storeIp: bcl 20,31,$+4 ; generateStackmap() ; mflr TMP ; push TMP]
mflr r0; stdu r0,-8(r1)          ; save LR (rule 2)
stdu FP,-8(r1); mr FP,r1         ; x64: push rbp; mov rsp,rbp
stdu CTX,-8(r1)                  ; spill context at -8(FP)
rldicr r1,r1,0,59                ; 16-align
stdu r1,-112(r1)                 ; ELFv1 frame: 48 header + 64 param save
                                 ;  (callee stores ITS LR/CR at our 16/8(r1);
                                 ;   40(r1) = TOC slot used by emitCallCFunction)
asmLoadTls TMP, threadTpoff ; ld TMP,stackFramesTail(TMP) ; std FP,exit(TMP)
emitCallCFunction(fn)            ; li64 r11,descriptor; std r2,40(r1);
                                 ; ld r0,0(r11); ld r2,8(r11); mtctr r0;
                                 ; ld r11,16(r11); bctrl; ld r2,40(r1)
ld CTX,-8(FP) ; mr r1,FP ; ld FP,0(r1); addi r1,r1,8
ld r0,0(r1); mtlr r0; addi r1,r1,8   ; restore LR
[storeIp: addi r1,r1,8]
```
C result arrives in r3 (== our result reg, mirroring RAX).

**PrimitiveResult (16B) under ELFv1 returns via HIDDEN sret pointer in r3**,
args shift right (PORT_ME(elfv1-sret)). generateCCallPrimitive:
emitCCallPrimArgs loads Smalltalk args into r4..r9 (shifted) and points r3 at
a 16-byte sret buffer INSIDE the 112-byte ABI frame generateCCall creates —
use offsets 96..111(r1) (top of the param-save area, caller-owned, callee
only reads its actual ≤6 param dwords). emitPrimResultCheck then reloads
value/failed from that buffer... **CAREFUL**: generateCCall restores r1 from
FP BEFORE the result check runs (x64 decodes RAX:RDX after restore). The sret
buffer address must be recomputed relative to nothing — instead have
emitCCallPrimArgs place the buffer at a FP-relative slot: NO — simplest
correct scheme: ELFv1 hook order inside generateCCallPrimitive is
  emitCCallPrimArgs (marshal args r4..; r3 = r1-relative sret slot inside the
  ABI frame — but computed AFTER generateCCall's stdu, i.e. the hook must
  emit AFTER frame setup)...
**Decision**: give Ppc64Abi's emitCCallPrimArgs the x64 signature but emit
the sret setup as `addi r3, r1, 96` and REQUIRE (assert in comments) that
generateCCall's frame shape (112 below aligned r1) is already established at
hook time — i.e. ppc64's generateCCallPrimitive calls the hook INSIDE its own
copy of the CCall sequence, not before it like x64. The Smalltalk args are at
`(i)*8(FP+16)`? NO — for prims the args are at r1-relative BEFORE the CCall
frame; the hook receives them FP-relative after `mr FP,r1`: Smalltalk arg i
lives at (i+2)*8(FP) (above saved-LR at FP+8? recompute: prim entry: r1 →
args (arg0 at 0(r1), frameless). CCall prologue pushes LR then FP → after
`mr FP,r1`: arg i at (i+2)*8(FP). Marshal from there. failed check: after
generateCCall tears down (r1 back to prim entry state), the sret buffer is
GONE (below r1, clobberable by signals!) — so emitPrimResultCheck must read
value+failed INTO regs BEFORE teardown. **Final decision**: ppc64 does NOT
reuse generateCCall inside generateCCallPrimitive; it emits a fused
CCall+marshal+decode sequence in PrimitivesPpc64.c (one place, still golden-
able), keeping the Abi hooks for the marshal/decode fragments. Read r3=value,
r4=failed from the buffer right after bctrl (before frame teardown), then
tear down, then `cmpdi r4,0; bne fail`.

## C→JIT entry (ELFv1 function descriptors)

C code calls JIT code in EXACTLY ONE place: Entry.c invoking the
SmalltalkEntry stub (`entry(method, nativeEntry, args, thread)`). Under ELFv1
a C function pointer is a DESCRIPTOR pointer, so casting raw insts breaks.
Seam: `vm/jit/TargetEntry.h` — `Value targetCallSmalltalkEntry(void *stubInsts,
void *arg0, void *arg1, Value *args, Thread *thread)`; x64/Bind: direct cast;
elfv1/Bind: build `{insts, current r2 (register asm), 0}` descriptor on the C
stack and call through it. Everything else (lookup cache, R11/r12 targets,
LookupCache.codes[]) stays RAW insts addresses — JIT-to-JIT calls don't use
descriptors. (ELFv2 later: raw cast works — gcc puts the target in r12.)

## Baked pointers in code (GC patching + IP fixups)

x64 bakes imm64 in movabs; GC loops (Scavenger.c:532, GarbageCollector.c:334)
read/patch `*(Value*)(insts+offset)` directly. ppc64 uses the fixed
5-instruction li64; no contiguous imm64 exists. Seam:
`targetReadCodePointer(const uint8_t*)` / `targetWriteCodePointer(uint8_t*,
uint64_t)` declared in jit/TargetAssembler.h, x64 = memcpy, ppc64 = li64
halfword decode/patch (asmLi64Patch). Refactor both GC loops to
read→process→write-back (scavenger needs a local Value temp for
processTaggedPointer). pointersOffsets point at the li64 sequence START.
ASM_FIXUP_IP (BlockOnException handler ip): Assembler.h asmBindFixup case 8
does a raw storeU64 — ppc64 registers the fixup with `size ==
ASM_FIXUP_SIZE_CODE_PTR (20)` and asmBindFixup routes that case to
targetWriteCodePointer. Flush icache after patching (already done).

## Endianness rules for emitters (BE!)

NEVER translate an x64 full-word access over sub-word C fields literally:
- AllocateStub header init: x64 stores the 64-bit masked hash at offset 8
  (LE: hash u32 + zeroed unused/payload/vars/tags). BE: `stw hash,8(obj)` +
  `stw 0,12(obj)` — two 32-bit stores, byte-exact on BE.
- generateHashPrimitive: x64 loads word@8 & 0xFFFFFFFF. BE: `lwz r3,7(rDI)`
  (tagged ptr, hash bytes 8-11) then `sldi r3,r3,2` to tag.
- ALL sub-word fields (InstanceShape.{size,varsSize,...}, CompiledCodeHeader
  .argsSize/.contextSize, Heap.safepointRequested int low-byte test) use
  natural-width loads at the C offsetof: lbz/lhz/lwz — endian-correct by
  construction. safepointRequested is an `int` (or _Bool?) — x64 tests its
  LOW byte at offset+0 (LE); BE low byte is at offset+3 for int32 — USE lwz +
  cmpwi 0 instead (word compare, endian-neutral). CHECK the actual field type
  when porting (Heap.h).
- Full-Value fields (class, size, vars, tlab top/end...) are plain ld/std.

## Arithmetic / flags mapping

- tag test (`test reg,3` + jnz): `andi. r0,reg,3; bne cr0,miss` (andi. sets CR0).
- pointer-tag tests: VALUE_POINTER=1, NEW_SPACE_TAG=8, TAG_REMEMBERED=32 all
  fit andi./lbz+andi.
- unsigned compares (COND_ABOVE...): cmpld/cmpldi; signed: cmpd/cmpdi.
- overflow-checked add/sub/mul/neg (tagged ints): `addo./subfo./mulldo./nego.`
  + `bso miss` on CR0.SO. XER[SO] is STICKY: fast path does NOT clear it; a
  stale SO only causes a false-positive branch-to-dispatch (correct, slower).
  Every miss/dispatch path emits `li r0,0; mtxer r0` (mtspr XER) to re-arm.
- div: divd on TAGGED operands = untagged quotient (x64 idiv trick); retag
  `sldi 2`. No SIGFPE on POWER, but the zero-divisor check stays (result
  undefined). INT_MIN/-1 impossible for tagged values.
- mod/rem: divd + mulld + subf.
- shifts: sradi (untag), sld/srd (reg amounts; ≥64 → 0, semantic divergence
  from x64's mod-64 accepted), `andi r,r,~3`-equivalent via rldicr clear.
- dec+jnz loops: `addic. rX,rX,-1; bne cr0` (addic. sets CR0).
- lea rip (storeIp): `bcl 20,31,$+4; mflr TMP` (stackmap emitted between so
  ic == the LR value's offset).
- Float compare NaN: fcmpu crf → FU bit = unordered (BI=crf*4+3), branch
  cleanly (better than x64's PF dance).

## Stubs

- SmalltalkEntry: entry hooks (already golden) + EntryStackFrame protocol
  translated 1:1; arg-copy loop uses r14 (inside the saved area) with
  pointer-walk instead of scaled index; call target: `mtctr regEntry; bctrl`
  with r12 = nativeEntry first (VM convention).
- Allocate: same structure; TLAB fields ld/std; header init per endianness
  rule; loops via addic./bne; scaled addressing via sldi+ldx/stdx or pointer
  walks.
- Lookup: hash (srdi 4, andi/rlwinm mask 4095... LOOKUP_CACHE_SIZE-1=4095
  fits andi. NO: andi takes UNSIGNED 16-bit — 4095<<3 for the scaled index?
  x64 uses SS_8 scaled index into 3 arrays: ppc64: `rldic r5,r5,3,...` to
  mask-and-scale in one rotate, or andi. then sldi 3 + ldx). classes[] /
  selectors[] cmp via ldx, codes[] load into r12.
- DNU: translated as-is (selector r3, argsSize r5, jump `mtctr; bctr`).

## Safepoint poll / write barrier

Direct translations; the flag test per the endianness rule; slow paths use
the Abi spill helpers (Ppc64Abi gets abiEmitCallerSavedSave/Restore emitting
one stdu-frame + std/ld list — POWER has no push/pop, so the helper makes ONE
frame of spillCount*8 rounded to 16, keeping r1 16-multiple... NOT required
mid-JIT (only at C calls) but cheap and tidy).

## Bring-up order (inside task #9)

1. Encoder batch 2 (lbz/lhz/stb/sth/ldx/stdx/lbzx/stbx, cmpd/cmpld/cmpldi,
   add/subf/addo./subfo./mulldo./nego./neg, mulld/divd, and/or/xor + Rc
   variants, andi., addic., sradi/srdi/clrldi via rldicl, sld/srd/srad,
   mtxer, fadd/fsub/fmul/fdiv/fcmpu, bso/bns helpers, bcl 20,31 + push/pop/
   call composite helpers) + golden+oracle.
2. Shared seams: targetRead/WriteCodePointer + GC loop refactor + fixup
   size-20 route + TargetEntry.h (x64 gate must stay green).
3. StubCodePpc64.c + PrimitivesPpc64.c + CodeGeneratorPpc64.c translation.
4. Golden the 5 x64-mirror sequences (loadtls/ccall/storecheck/ccallprim/
   entry stub) + oracle.
5. Cross-build BE, qemu-user: `st -e '3 + 4'` (first JIT execution!), then
   bootstrap `-b smalltalk`, then tests. Debug loop: qemu -g + gdb.
6. build.sh in a BE rootfs (debootstrap debian-ports under binfmt) — needs
   cmake in the rootfs.
7. ppc64le mirror (LE emitters, ELFv2: no descriptors, TOC save 24(r1),
   PrimitiveResult in r3:r4 like SysV — SIMPLER; localentry note).

## Open items / verify-on-port

- Heap.safepointRequested field type (endianness of the byte test).
- OBJECT_HEADER hash zeroing: also zero `unused` byte (stw covers 8-11 =
  hash only; bytes 12-15 = unused/payload/vars/tags — the second stw zeroes
  them, then the stub's later byte-stores set payload/vars; VERIFY tags byte
  ends 0 like x64 (movq stored 0 there via zero-extend — yes, and x64 never
  re-set tags in the fast path → BE two-stw scheme matches).
- CodeDescriptors.h packing (bits 16-63) — arch-neutral, verify no
  byte-order assumption.
- qemu-user pagesize (osPageSize under qemu) — non-JIT tests already pass.
- VSX/v20-31 unsaved in fiber switch (documented; gcc default for BE debian
  ports baseline is pre-VSX — verify -mcpu default when building in rootfs).
