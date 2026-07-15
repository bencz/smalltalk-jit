#!/usr/bin/env bash
# Independent oracle for the ppc64 (big-endian) golden emission vectors:
# the SAME instruction sequences as vm/tests/EmitGoldenPpc64.c, written in
# GNU assembler syntax, assembled with the debian cross binutils inside an
# x86_64 container, .text extracted raw, and byte-compared on the host
# against the ST_PPC64_EMIT_TEST=print output of the build's own emitters.
#
#   scripts/ppc64/golden-oracle.sh [build-dir]   (default: ./build)
#
# Run this BEFORE pinning regenerated vectors into EmitGoldenPpc64Expected.h.
# The .s below is maintained BY HAND on purpose — an independent derivation
# from the Power ISA is exactly what makes it an oracle. Podman gotchas
# (PORTING.md): --platform always explicit; mounts with :z, never :Z.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/build}"
WORK="$BUILD/golden-oracle"
mkdir -p "$WORK"

echo "=== capturing the emitters' own bytes (ST_PPC64_EMIT_TEST=print) ==="
LD_LIBRARY_PATH="$BUILD" ST_PPC64_EMIT_TEST=print "$BUILD/st" </dev/null > "$WORK/print.txt"

echo "=== writing the hand-maintained reference .s ==="
{
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
} > "$WORK/ref.s"

echo "=== cross-assembling with debian binutils (x86_64 container) ==="
podman run --rm --platform linux/amd64 -v "$WORK":/w:z docker.io/library/debian:stable /bin/bash -c "
	set -eu
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends binutils-powerpc64-linux-gnu >/dev/null
	powerpc64-linux-gnu-as /w/ref.s -o /tmp/ref.o
	powerpc64-linux-gnu-objcopy -O binary --only-section=.text /tmp/ref.o /w/ref.bin
	powerpc64-linux-gnu-objdump -d /tmp/ref.o > /w/ref.dis
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
