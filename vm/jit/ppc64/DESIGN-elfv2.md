# ppc64le (LE/ELFv2) backend design: the DELTAS vs the ppc64 BE backend

**Read `vm/jit/ppc64/DESIGN.md` first, it is the spec.** Register roles, frame
layout, the arithmetic/flags mapping, the stub structure and the seven bring-up
bugs are IDENTICAL here and are not repeated. This file records only what
differs, and why.

The one-line framing that resolves almost every question:

> **ppc64le = the ppc64 ISA + the x64 endianness.**
> ISA/frame question: copy `vm/jit/ppc64/`. Endianness question (object header,
> hash): **`vm/jit/x64/` is the reference, NOT the BE backend.**

Since the unification there is ONE backend (`vm/jit/ppc64/`) serving both
byte orders: the word layout is the WordBe.h/WordLe.h selector in
AssemblerPpc64.h, the conventions below live in `abi/elfv2/` behind the
Ppc64Abi vtable, and the POWER8 floor is `cpu/CpuBindLe.c`. A POWER fix is
written once. This file remains the record of the ELFv2 facts and how they
were derived.

## ELFv2 facts, DERIVED FROM THE TOOLCHAIN rather than from documentation

Probe: a C file mirroring `vm/runtime/Primitives.h`'s `PrimitiveResult` compiled
`-O2 -S` with debian's `powerpc64le-linux-gnu-gcc` (14.x, `.machine power8`,
`.abiversion 2`). **Control: the same probe through `powerpc64-linux-gnu-gcc`
reproduced every known ELFv1 fact** (`.opd` descriptor `.quad
.L.fn,.TOC.@tocbase,0`; hidden sret `addi 3,1,112` with args shifted right;
`ld 9,120(1)`/`ld 3,112(1)` reading the buffer back; `std 2,40(1)`; the
`ld 10,0(3); ld 11,16(3); mtctr 10; ld 2,8(9)` dance), which is what makes the
LE answers trustworthy rather than remembered.

### 1. PrimitiveResult (16 B, non-float aggregate) returns in r3:r4

No hidden sret pointer, arguments NOT shifted: the SysV/x64 shape.
`PORT_ME(elfv1-sret)` simply **does not exist on this backend**.

```
probeIndirectPrim:                  ; PrimitiveResult (*fn)(Value,Value,Value)
    mr 12,3 ; mtctr 12
    mr 3,4 ; addi 5,4,2 ; addi 4,4,1    ; args in r3,r4,r5, unshifted
    std 0,16(1) ; stdu 1,-32(1) ; std 2,24(1)
    bctrl
    ld 2,24(1)
    cmpdi 0,4,0 ; bne 0,.L14            ; failed = r4 ... value = r3
```

Consequences: `emitCCallPrimArgs`/`emitPrimResultCheck` are REAL (not `FAIL()`),
and **`generateCCallPrimitive` uses the normal `generateCCall` + hooks path like
x64** instead of the fused CCall+marshal+decode sequence that ELFv1's sret forced
in `PrimitivesPpc64.c`. That is a difference in SHAPE, not in values: the single
biggest structural divergence between the two POWER backends.

### 2. C-call frame: 32-byte header, TOC at 24(r1), NO parameter save area

```
probeIndirect8:                     ; Value (*fn)(Value x8), the generateCCall shape
    mflr 0
    mr 12,3 ; mtctr 12
    ... args -> r3..r10 ...
    std 0,16(1)                     ; LR into the CALLER's header slot
    stdu 1,-32(1)                   ; 32-byte frame, even with EIGHT args
    std 2,24(1)                     ; TOC save
    bctrl
    ld 2,24(1)                      ; TOC restore
    addi 1,1,32
```

- Header 32 B: back chain 0, CR 8, LR 16, TOC 24. (ELFv1: 48 B, TOC at 40.)
- **`paramSaveArea = 0`**, proven: gcc allocates no home space for an
  8-argument call. (ELFv1 mandates 64 B.) The area is optional under ELFv2 and
  our C callees are prototyped and non-variadic with <= 8 args.
- gcc DOES save and restore r2 around the indirect call, so we do too.

### 3. Indirect calls put the target in r12, which is already our `TGT`

`mr 12,3; mtctr 12`. ELFv2 requires r12 = the callee's GLOBAL ENTRY address so
the callee's prologue can derive its own r2. This is a **harmony, not a
collision**: the VM's send-target register is already r12
(`AssemblerPpc64.h`: `TGT = R12_PPC`).

`emitCallCFunction` therefore collapses from ELFv1's 8 instructions to 5:

```
li64 r12, cFunction     ; the fixed 5-instruction shape
std  r2, 24(r1)
mtctr r12
bctrl
ld   r2, 24(r1)
```

WARNING: it clobbers `TGT` (r12), where the ELFv1 hook clobbers `TMP` (r11).
Every `emitCallCFunction` call site must have `TGT` dead. The existing rule
already covers this (`generateCCallPrimitive` saves `TGT` into r15, the x64
R11/R13 dance, before the call), but it is an audit item on every copied site
rather than a freebie.

### 4. The global entry derives r2 from r12: the one NEW design problem

```
0:  addis 2,12,.TOC.-.LCF0@ha
    addi  2,2,.TOC.-.LCF0@l
    .localentry probeIndirect8,.-probeIndirect8
```

A C function entered at its GLOBAL entry with a wrong r12 therefore computes a
garbage TOC. This breaks the naive `fiberPrimeStack`: ELFv1's prime dereferences
the descriptor and seeds the frame's r2 slot, but under ELFv2 `entry` is already
the code address and **the primed frame must deliver r12 = entry** when the
switch returns into it.

Chosen approach: `fiberSwitchElfV2` sets `r12` from the LR value it is about to
return to (`mr 12,0` next to `mtlr 0`), one instruction, no extra frame slot and
no seeding. It is uniformly correct:

- primed fiber: the LR slot holds `entry`, so r12 = entry, exactly what the
  global-entry prologue needs;
- normal resume: the LR slot holds a return address inside the C caller of the
  switch, and r12 is volatile and dead there, so writing it is inert.

The prime consequently does NOT need to seed r2 (the global-entry prologue
establishes it from r12), which also keeps the prime host-independent: reading
the live r2 from C would need `register asm("r2")` and would not compile on the
x86 golden host. Rejected alternative: an asm trampoline
(`mr r12,r14; mtctr r12; bctr`) seeded through a nonvolatile, which works but
adds a symbol and a seeding step for no gain.
Checked natively by the golden's `checkFiberPrimeLayout()`, then by execution.

### 5. `targetCallSmalltalkEntry` is a plain cast

No descriptors, so the x64 shape (`vm/jit/x64/Abi.c`) applies, and none of
ELFv1's `volatile` plus asm-barrier dance (which existed only because GCC
dead-stored the synthesized stack descriptor).

### 6. HAZARD: the LE baseline is `power8`, so VSX is IN, unlike BE

`powerpc64le-linux-gnu-gcc` defaults to `.machine power8`; the BE cross defaults
to `.machine power4` (pre-VSX). The whole ppc64le ecosystem requires POWER8+.

The fiber switch saves r14-r31 and f14-f31 but **NOT v20-v31/VSX** (a documented
gap, `PORT_ME` in PORTING.md). On BE that gap was theoretical because the
baseline had no VSX. **On LE the compiler may legitimately allocate a
nonvolatile vector register across a call to `fiberSwitchAsm`, so the exposure is
real**, though still low probability: it needs vectorized code whose vector value
is live across the switch, after the volatiles are exhausted. Saving v20-v31
costs 192 bytes of frame and is unconditionally encodable at the POWER8
baseline. Not done yet, and flagged here so the decision is explicit rather than
inherited by accident.

## Frame constants (`abi/elfv2/FiberElfV2.h`)

| | ELFv1 (BE) | ELFv2 (LE) |
|---|---|---|
| header size | 48 | 32 |
| back chain | 0 | 0 |
| CR save | 8 | 8 |
| LR save | 16 | 16 |
| TOC save | 40 | 24 |
| param save area | 64 (mandatory) | 0 (optional, unused) |
| GPR r14-r31 save | 48 | 32 |
| FPR f14-f31 save | 192 | 176 |
| nonvolatile frame | 336 | 320 |

## Endianness deltas in the codegen: there are NONE (verified, not assumed)

This was the expected hot spot, and it evaporated on inspection. The BE
backend's own rule ("sub-word fields use natural-width accesses at the C
offsetof, endian-correct by construction") already made every candidate site
byte-order-neutral, so the LE codegen is a copy of the BE one modulo ELFv2:

- **AllocateStub header init**: `stw hash,8(obj)` plus `stw 0,12(obj)`. Two
  natural-width stores at the field's own offset are correct in BOTH orders,
  and semantically identical to x64's single `movq` of the 0xFFFFFFFF-masked
  hash (which lands hash in bytes 8-11 and zeros 12-15). It is the x64 form
  that is little-endian-specific, not the POWER form that is big-endian-specific.
  KEEP the two stores here.
- **`generateHashPrimitive`**: `lwz` at `varOffset(RawObject, hash)` = disp 7
  from the tagged receiver (EA = obj+8) on BOTH backends. `hash` is a `uint32_t`
  at offset 8 and is NOT endian-mirrored (only `CompiledCodeHeader` is), so a
  32-bit load at its offset yields the field in either order.
  (Historical note: the BE backend shipped this with disp 8, reading obj+9. It
  survived every test because a wrong-but-deterministic hash still behaves like
  a hash; found and fixed while porting this site to LE. It is the exact class
  of bug the `varOffset`/`offsetof` discipline exists to prevent, so prefer the
  macro over a hand-computed literal, always.)
- `Heap.safepointRequested`: `lwz` plus `cmpwi 0` is endian-neutral, inherited.
- Every sub-word field reached through `offsetof`/`varOffset` recomputes itself.

**`CompiledCodeHeader` is the sharpest trap in this port.**
`vm/core/CompiledCode.h` MIRRORS the struct under `TARGET_BIG_ENDIAN` to keep the
`tag` byte in the low position. LE takes the `#else` (x64) branch, so **the same
field name has a different byte offset than on BE**: `primitive` sits at offset 6
on LE and 0 on BE. Any line copied from `vm/jit/ppc64/` that hardcodes a header
offset instead of using `offsetof` is silent heap corruption. Audit every numeric
literal in the copied files.

## Assembler: exactly 6 endian-dependent functions

`AssemblerPpc64le.h` is a full copy of `AssemblerPpc64.h`; 95 of its 101
functions pack bit-fields into a `uint32_t` and are byte-order-neutral by
construction. Only these touch raw code bytes:

`ppcLoadWord`, `ppcStoreWord` (LE byte order), `asmPpcEmitWord` and
`asmPpcLabelBind` (correct for free, they funnel through the two above), and
`asmLi64Read`/`asmLi64Patch` (re-indexed).

Emission stays **explicitly little-endian** (hand shifts), NOT native-order
`storeU32`: the golden compiles and runs on the x86 host as well as on the
target, and explicit order is exactly what makes the TU host-independent.

`asmLi64Read`/`asmLi64Patch` re-indexing: for instruction `k` at byte offset
`4k`, BE stores word `W` as `[W>>24, W>>16, W>>8, W]` and LE as
`[W, W>>8, W>>16, W>>24]`:

| | BE | LE |
|---|---|---|
| opcode check byte | `seq[4k]>>2` | `seq[4k+3]>>2`, i.e. 3, 7, 11, 15, 19 |
| imm16 high byte | `seq[4k+2]` | `seq[4k+1]` |
| imm16 low byte | `seq[4k+3]` | `seq[4k]` |
| the 8 imm bytes (hi,lo) | (2,3) (6,7) (14,15) (18,19) | (1,0) (5,4) (13,12) (17,16) |

This is the highest-risk hand translation in the port (silent wrong code, and the
GC patches pointers through it). It is pinned by the golden's `checkLi64Patch()`
plus the binutils oracle before anything else runs. A free cross-check while
bringing it up: the LE word bytes must be the BE golden's bytes reversed within
each word, e.g. the `sldi` word is `79 4A 07 C6` on BE and `C6 07 4A 79` on LE.

## Inherited obligations that have no compiler diagnostic

- **Frameless `-1` de-bias** in `fillVar`: `vm/jit/RegisterAllocator.c:102`
  applies x86's `+1` (the return address `call` pushes) even on the frameless
  path; POWER keeps it in LR, so arg `i` really sits at `i*8(r1)`. The
  compensation is ARCH-side and must be re-implemented here. Forgetting it reads
  one slot too high (BE bring-up bug #2).
- **`varReg()`** mapping `SPILLED_REG` (-1) to r4, plus the negative-register
  ASSERTs in the D/DS/X-form builders: `-1 & 31` = r31 = `FP` (BE bug #4).
- **`asmPpcLabelBind`, never the generic `asmLabelBind`**: `vm/jit/Assembler.h`'s
  binder is x86-shaped (displacement relative to the END of the instruction),
  while POWER branches are relative to their OWN address. `label->size = 4` makes
  the generic `case 4` look applicable: wrong code, no diagnostic.
- **`generateCCall` pushes LR BEFORE FP**: `[FP+8]` must hold the storeIp IP that
  the GC's frame walker reads as `parentIc` (BE bug #1: a stash at a scratch slot
  corrupted every scavenge inside a C call).
- **`targetReadCodePointer`/`targetWriteCodePointer`** must be defined here (the
  GC patches the split `li64` immediate through them); the skeleton lacked them
  entirely.
