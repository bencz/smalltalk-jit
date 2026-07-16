#!/usr/bin/env bash
# Independent oracle for the POWER golden emission vectors: the SAME
# instruction sequences as vm/tests/EmitGoldenPpc64{,le}.c, written in GNU
# assembler syntax, assembled with the debian cross binutils inside an x86_64
# container, .text extracted raw, and byte-compared on the host against the
# ST_PPC64_EMIT_TEST / ST_PPC64LE_EMIT_TEST print output of the build's own
# emitters.
#
#   scripts/ppc64/golden-oracle.sh <be|le> [build-dir]   (default build: ./build)
#
# Run this BEFORE pinning regenerated vectors into EmitGoldenPpc64*Expected.h.
# The .s bodies below are maintained BY HAND on purpose: an independent
# derivation from the Power ISA is exactly what makes this an oracle. Both
# byte orders are checked the same way, since the comparison is over raw bytes
# and the endianness lives entirely inside the emitters and `as`.
#
# Podman gotchas (PORTING.md): --platform always explicit (the container is
# x86_64 for BOTH modes, only the cross toolchain changes); mounts with :z,
# never :Z.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODE="${1:?usage: golden-oracle.sh <be|le> [build-dir]}"
BUILD="${2:-$ROOT/build}"

case "$MODE" in
	be)
		EMIT_VAR=ST_PPC64_EMIT_TEST
		BINUTILS=binutils-powerpc64-linux-gnu
		PREFIX=powerpc64-linux-gnu
		;;
	le)
		EMIT_VAR=ST_PPC64LE_EMIT_TEST
		BINUTILS=binutils-powerpc64le-linux-gnu
		PREFIX=powerpc64le-linux-gnu
		;;
	*) echo "usage: $0 <be|le> [build-dir]"; exit 2 ;;
esac

WORK="$BUILD/golden-oracle-$MODE"
mkdir -p "$WORK"

echo "=== capturing the emitters' own bytes ($EMIT_VAR=print) ==="
LD_LIBRARY_PATH="$BUILD" env "$EMIT_VAR=print" "$BUILD/st" </dev/null > "$WORK/print.txt"

echo "=== writing the hand-maintained reference .s ($MODE) ==="

# Cases shared by both backends: the raw encoders, whose bytes are the same
# instruction words in either byte order. Kept in ONE place so a BE/LE
# divergence here would be a real encoder bug rather than a transcription slip.
writeSharedArith() {
	cat <<'EOF'
# case: addi/addis/li/lis/mr/rldicr/cmpdi
	addi 5,1,64
	addi 5,1,-64
	addis 5,2,0x1234
	li 0,0
	li 31,-1
	lis 7,-2
	mr 3,31
	nop
	sldi 9,9,32
	rldicr 7,8,12,50
	cmpdi 0,3,0
	cmpdi 7,5,-1
# case: mflr/mtlr/mtctr/mfcr/mtcrf/blr/bctr/bctrl
	mflr 0
	mtlr 0
	mtctr 0
	mfcr 5
	mtcrf 0xff,5
	blr
	bctr
	bctrl
# case: b/bc labels (forward + backward)
	cmpdi 0,3,0
	beq .Lfwd
	b .Lfwd2
.Lback:
	nop
	bne .Lback
.Lfwd:
	nop
.Lfwd2:
	b .Lback
EOF
}

writeSharedTail() {
	cat <<'EOF'
# case: lbz/stb/lhz/sth/ldx/stdx/lbzx/stbx
	lbz 5,7(3)
	stb 5,15(3)
	lhz 5,6(4)
	sth 5,6(4)
	ldx 6,3,5
	stdx 6,3,5
	lbzx 6,3,5
	stbx 6,3,5
# case: add/subf/neg/mulld/divd + OE.Rc/andi./addic.
	add 5,3,4
	addo. 5,3,4
	subf 5,4,3
	subfo. 5,4,3
	neg 5,3
	nego. 5,3
	mulld 5,3,4
	mulldo. 5,3,4
	divd 5,3,4
	and 5,3,4
	xor 5,3,4
	andi. 0,3,3
	addic. 14,14,-1
# case: srdi/clrldi/sradi/sld/srd/srad/cmpd/cmpld/cmpldi
	srdi 5,3,4
	clrldi 5,3,56
	sradi 5,3,2
	sld 5,3,4
	srd 5,3,4
	srad 5,3,4
	cmpd 0,3,4
	cmpld 7,3,4
	cmpldi 1,3,4095
# case: fadd/fsub/fmul/fdiv/fcmpu/xer/bcl/push/pop/callreg
	fadd 0,0,1
	fsub 0,0,1
	fmul 0,0,1
	fdiv 0,0,1
	fcmpu 1,0,1
	li 0,0
	mtxer 0
	bcl 20,31,.+4
	stdu 3,-8(1)
	ld 3,0(1)
	addi 1,1,8
	mtctr 12
	bctrl
EOF
}

# ---- big-endian / ELFv1 -------------------------------------------------------
# Frame facts: 48-byte header, TOC save at 40(r1), 336-byte NV frame with GPRs
# at 48 and FPRs at 192; C functions are called through an .opd descriptor
# {entry, TOC, environ} loaded via r11.
writeRefBe() {
	cat <<'EOF'
	.text
# case: asmLi64(r10, 0x1122334455667788)
	lis 10,0x1122
	ori 10,10,0x3344
	rldicr 10,10,32,31
	oris 10,10,0x5566
	ori 10,10,0x7788
# case: ld/std/stdu/lwz/stw/lfd/stfd
	ld 4,16(1)
	ld 0,0(11)
	ld 2,8(11)
	ld 11,16(11)
	std 31,-8(1)
	std 2,40(1)
	stdu 1,-336(1)
	lwz 0,8(1)
	stw 0,8(1)
	lfd 14,192(1)
	stfd 31,328(1)
EOF
	writeSharedArith
	cat <<'EOF'
# case: elfv1 emitLoadTls(r11, 0x1234)
	addis 11,13,0
	addi 11,11,0x1234
# case: elfv1 emitLoadTls(r11, -0x12344)  [ha/lo rounding: -1<<16 - 9028]
	addis 11,13,-1
	addi 11,11,-9028
# case: elfv1 emitCallCFunction (descriptor at 0x1122334455667788)
	lis 11,0x1122
	ori 11,11,0x3344
	rldicr 11,11,32,31
	oris 11,11,0x5566
	ori 11,11,0x7788
	std 2,40(1)
	ld 0,0(11)
	ld 2,8(11)
	mtctr 0
	ld 11,16(11)
	bctrl
	ld 2,40(1)
# case: elfv1 entry save regs
	mflr 0
	std 0,16(1)
	mfcr 0
	stw 0,8(1)
	stdu 1,-336(1)
	std 2,40(1)
EOF
	for i in $(seq 0 17); do echo "	std $((14 + i)),$((48 + 8 * i))(1)"; done
	for i in $(seq 0 17); do echo "	stfd $((14 + i)),$((192 + 8 * i))(1)"; done
	echo "# case: elfv1 entry restore regs"
	for i in $(seq 0 17); do echo "	ld $((14 + i)),$((48 + 8 * i))(1)"; done
	for i in $(seq 0 17); do echo "	lfd $((14 + i)),$((192 + 8 * i))(1)"; done
	cat <<'EOF'
	ld 2,40(1)
	addi 1,1,336
	ld 0,16(1)
	mtlr 0
	lwz 0,8(1)
	mtcrf 0xff,0
EOF
	writeSharedTail
}

# ---- little-endian / ELFv2 -----------------------------------------------------
# Frame facts (all derived from gcc's own output, see vm/jit/ppc64le/DESIGN.md):
# 32-byte header, TOC save at 24(r1), 320-byte NV frame with GPRs at 32 and
# FPRs at 176; NO function descriptors, so a C call just puts the target in
# r12 (the ABI's global-entry register) and dispatches through CTR. The two
# CCALL-primitive cases have no BE counterpart: ELFv2 returns the 16-byte
# PrimitiveResult in r3:r4 with unshifted args, where ELFv1 needs a hidden
# sret pointer.
writeRefLe() {
	cat <<'EOF'
	.text
# case: asmLi64(r10, 0x1122334455667788)
	lis 10,0x1122
	ori 10,10,0x3344
	rldicr 10,10,32,31
	oris 10,10,0x5566
	ori 10,10,0x7788
# case: ld/std/stdu/lwz/stw/lfd/stfd
	ld 4,16(1)
	ld 0,0(11)
	ld 2,8(11)
	ld 11,16(11)
	std 31,-8(1)
	std 2,24(1)
	stdu 1,-320(1)
	lwz 0,8(1)
	stw 0,8(1)
	lfd 14,176(1)
	stfd 31,312(1)
EOF
	writeSharedArith
	cat <<'EOF'
# case: elfv2 emitLoadTls(r11, 0x1234)
	addis 11,13,0
	addi 11,11,0x1234
# case: elfv2 emitLoadTls(r11, -0x12344)  [ha/lo rounding: -1<<16 - 9028]
	addis 11,13,-1
	addi 11,11,-9028
# case: elfv2 emitCallCFunction (target 0x1122334455667788 in r12, no descriptor)
	lis 12,0x1122
	ori 12,12,0x3344
	rldicr 12,12,32,31
	oris 12,12,0x5566
	ori 12,12,0x7788
	std 2,24(1)
	mtctr 12
	bctrl
	ld 2,24(1)
# case: elfv2 emitCCallPrimArgs(5)  [frameless prim: arg i at i*8(r1), no +8]
	ld 3,0(1)
	ld 4,8(1)
	ld 5,16(1)
	ld 6,24(1)
	ld 7,32(1)
# case: elfv2 emitPrimResultCheck (r3:r4)
	cmpdi 0,4,0
	bne .Lprimfail
.Lprimfail:
# case: elfv2 entry save regs
	mflr 0
	std 0,16(1)
	mfcr 0
	stw 0,8(1)
	stdu 1,-320(1)
	std 2,24(1)
EOF
	for i in $(seq 0 17); do echo "	std $((14 + i)),$((32 + 8 * i))(1)"; done
	for i in $(seq 0 17); do echo "	stfd $((14 + i)),$((176 + 8 * i))(1)"; done
	echo "# case: elfv2 entry restore regs"
	for i in $(seq 0 17); do echo "	ld $((14 + i)),$((32 + 8 * i))(1)"; done
	for i in $(seq 0 17); do echo "	lfd $((14 + i)),$((176 + 8 * i))(1)"; done
	cat <<'EOF'
	ld 2,24(1)
	addi 1,1,320
	ld 0,16(1)
	mtlr 0
	lwz 0,8(1)
	mtcrf 0xff,0
EOF
	writeSharedTail
}

if [ "$MODE" = be ]; then writeRefBe > "$WORK/ref.s"; else writeRefLe > "$WORK/ref.s"; fi

echo "=== cross-assembling with debian binutils ($PREFIX, x86_64 container) ==="
podman run --rm --platform linux/amd64 -v "$WORK":/w:z docker.io/library/debian:stable /bin/bash -c "
	set -eu
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends $BINUTILS >/dev/null
	$PREFIX-as /w/ref.s -o /tmp/ref.o
	$PREFIX-objcopy -O binary --only-section=.text /tmp/ref.o /w/ref.bin
	$PREFIX-objdump -d /tmp/ref.o > /w/ref.dis
"

echo "=== comparing emitter bytes against the oracle ==="
python3 - "$WORK/print.txt" "$WORK/ref.bin" <<'PY'
import re, sys

printText = open(sys.argv[1]).read()
oracle = open(sys.argv[2], "rb").read()

cases = []
for m in re.finditer(r"// (.+?) \(\d+ bytes\)\nstatic const uint8_t (\w+)\[\] = \{([^}]*)\};", printText):
    name, array, body = m.groups()
    cases.append((name, bytes(int(t, 16) for t in re.findall(r"0x([0-9A-Fa-f]{2})", body))))

emitted = b"".join(b for _, b in cases)
if emitted == oracle:
    for name, b in cases:
        print(f"oracle ok: {name} ({len(b)} bytes)")
    print(f"ORACLE PASSED: {len(cases)} cases, {len(emitted)} bytes byte-identical to cross-as output")
    sys.exit(0)

print(f"ORACLE MISMATCH: emitters {len(emitted)} bytes, oracle {len(oracle)} bytes")
offset = 0
for name, b in cases:
    chunk = oracle[offset:offset + len(b)]
    if chunk != b:
        diff = next((i for i in range(min(len(b), len(chunk))) if b[i] != chunk[i]), len(chunk))
        print(f"  case '{name}': first difference at +{diff}")
        print(f"    emitter: {b[diff & ~3:(diff & ~3) + 4].hex()}")
        print(f"    oracle:  {chunk[diff & ~3:(diff & ~3) + 4].hex()}")
    offset += len(b)
sys.exit(1)
PY
