#!/usr/bin/env bash
# ppc64 validation without target hardware, for BOTH byte orders:
#   1. cross-compile STATIC ppc64 test binaries at native speed inside an
#      x86_64 debian container (debian cross toolchains ship a full libc,
#      unlike Fedora's);
#   2. execute them on the host — binfmt + qemu-user-static run foreign statics
#      transparently.
#
#   scripts/ppc64/cross-test.sh be   # big-endian, ELFv1  (the endianness fire test)
#   scripts/ppc64/cross-test.sh le   # little-endian, ELFv2
#
# Host prerequisite (once): sudo dnf install -y qemu-user-static-ppc
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODE="${1:?usage: cross-test.sh <be|le>}"

case "$MODE" in
	be)
		TARGET_ARCH=ppc64
		TOOLCHAIN="gcc-powerpc64-linux-gnu libc6-dev-ppc64-cross"
		CROSS_CC=powerpc64-linux-gnu-gcc
		BINFMT=qemu-ppc64
		;;
	le)
		TARGET_ARCH=ppc64le
		TOOLCHAIN="gcc-powerpc64le-linux-gnu libc6-dev-ppc64el-cross"
		CROSS_CC=powerpc64le-linux-gnu-gcc
		BINFMT=qemu-ppc64le
		;;
	*) echo "usage: $0 <be|le>"; exit 2 ;;
esac
OUT="$ROOT/build-$TARGET_ARCH"
mkdir -p "$OUT"

if ! ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | grep -q "$BINFMT"; then
	echo "no $BINFMT binfmt handler. run once: sudo dnf install -y qemu-user-static-ppc"
	exit 1
fi

echo "=== cross-building static $TARGET_ARCH binaries in an x86_64 debian container ==="
podman run --rm --platform linux/amd64 -v "$ROOT":/src:z docker.io/library/debian:stable /bin/bash -c "
	set -eu
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends $TOOLCHAIN >/dev/null
	TARGET_ARCH=$TARGET_ARCH CC=$CROSS_CC STATIC=1 \
		/src/scripts/ppc64/build-nocmake.sh /src/build-$TARGET_ARCH
"

echo "=== running $TARGET_ARCH binaries on the host via binfmt/qemu ==="
file "$OUT/st" | head -1
echo "== TokenizerTest:"; "$OUT/TokenizerTest" && echo OK
echo "== ST_SAFEPOINT_TEST:"; ST_SAFEPOINT_TEST=1 "$OUT/st" && echo OK
echo "== ST_TLAB_TEST:"; ST_TLAB_TEST=1 "$OUT/st" && echo OK
echo "== ST_SNAPSHOT_FORMAT_TEST:"; ST_SNAPSHOT_FORMAT_TEST=1 "$OUT/st" && echo OK
# SmallFloat64 rotation encoding, re-proven on a genuine big/little-endian target
echo "== ST_SMALLFLOAT_TEST:"; ST_SMALLFLOAT_TEST=1 "$OUT/st" && echo OK
# The arch's own emission golden: the same pinned vectors the x86 host runs
# natively, re-checked on a genuinely big/little-endian target under qemu. Each
# backend's Bind TU aliases ST_ABI_EMIT_TEST to its own golden.
echo "== ST_ABI_EMIT_TEST:"; ST_ABI_EMIT_TEST=1 "$OUT/st" && echo OK
echo "ALL $MODE TESTS PASSED ($TARGET_ARCH under qemu-user)"

# ---- CPU feature-detection sanity on emulated models -----------------------
# The decode must claim the right ISA level under each QEMU_CPU (see
# checkCpuDecode for the fabricated-words golden; this is the END-TO-END check
# through the real kernel hwcap words qemu reports), and the float suite must
# pass on BOTH sides of the isPower8 (ISA 2.07) gate: mtvsrd/mfvsrd vs the
# TLS memory path for GPR<->FPR moves.
if [ "$MODE" = be ]; then
	# NOTE: QEMU_CPU=970 cannot run this binary at all: the Debian cross glibc
	# requires POWER7 (a rootfs constraint, not a JIT one). power7 is the
	# oldest runnable model and exactly the case we need anyway: VSX present
	# but NO ISA 2.07, so the GPR<->FPR moves MUST take the memory path.
	# gprvsr is the DERIVED capability the emitter branches on: assert it on
	# both sides of the gate, not just the raw level bits.
	echo "== ST_CPU_INFO (power7 must NOT claim ISA 2.07; power9 must):"
	QEMU_CPU=power7 ST_CPU_INFO=1 ST_SMALLFLOAT_TEST=1 "$OUT/st" </dev/null | grep "ppc64 CPU"
	QEMU_CPU=power7 ST_CPU_INFO=1 ST_SMALLFLOAT_TEST=1 "$OUT/st" </dev/null | grep -q "power8=0" \
		|| { echo "FAIL: power7 claimed ISA 2.07"; exit 1; }
	QEMU_CPU=power7 ST_CPU_INFO=1 ST_SMALLFLOAT_TEST=1 "$OUT/st" </dev/null | grep -q "gprvsr=0" \
		|| { echo "FAIL: power7 claimed the GPR<->VSR moves"; exit 1; }
	QEMU_CPU=power9 ST_CPU_INFO=1 ST_SMALLFLOAT_TEST=1 "$OUT/st" </dev/null | grep "ppc64 CPU"
	QEMU_CPU=power9 ST_CPU_INFO=1 ST_SMALLFLOAT_TEST=1 "$OUT/st" </dev/null | grep -q "power8=1 power9=1" \
		|| { echo "FAIL: power9 must imply ISA 2.07"; exit 1; }
	QEMU_CPU=power9 ST_CPU_INFO=1 ST_SMALLFLOAT_TEST=1 "$OUT/st" </dev/null | grep -q "gprvsr=1" \
		|| { echo "FAIL: power9 must have the GPR<->VSR moves"; exit 1; }
else
	echo "== ST_CPU_INFO (the ppc64le floor is POWER8: gprvsr everywhere):"
	for cpu in power8 power9; do
		QEMU_CPU=$cpu ST_CPU_INFO=1 ST_SMALLFLOAT_TEST=1 "$OUT/st" </dev/null | grep "ppc64 CPU"
		QEMU_CPU=$cpu ST_CPU_INFO=1 ST_SMALLFLOAT_TEST=1 "$OUT/st" </dev/null | grep -q "gprvsr=1" \
			|| { echo "FAIL: $cpu must have the GPR<->VSR moves"; exit 1; }
	done
fi

# ---- the .st test surface under qemu ----------------------------------------
# Exactly the gap that let the boxed-result canonicity bug ship: the goldens
# and C self-tests above never exercise Smalltalk semantics on the target.
echo "== bootstrap + numeric .st tests under qemu ($TARGET_ARCH):"
IMG="/tmp/st-cross-$TARGET_ARCH.img"
"$OUT/st" -s "$IMG" -b smalltalk </dev/null >/dev/null
if [ "$MODE" = be ]; then
	# Bootstrap compiles the WHOLE kernel: the largest codegen the VM ever
	# does, and the case that shipped broken once. Under power7 the float
	# fast path takes the longer TLS memory path for GPR<->FPR moves, which
	# pushed Character class>>initialize past the old 64KB per-method
	# pointer-offset ceiling. Run the full bootstrap on the memory-path side
	# of the gate, then prove the image it built actually works there.
	echo "==   full bootstrap under QEMU_CPU=power7 (memory-path codegen):"
	IMG7="/tmp/st-cross-$TARGET_ARCH-power7.img"
	QEMU_CPU=power7 timeout 1800 "$OUT/st" -s "$IMG7" -b smalltalk </dev/null >/dev/null \
		|| { echo "FAIL: bootstrap under power7"; exit 1; }
	QEMU_CPU=power7 timeout 900 "$OUT/st" -s "$IMG7" -f tests/FloatTest.st </dev/null >/dev/null 2>&1 \
		|| { echo "FAIL: FloatTest.st on the power7-built image"; exit 1; }
	echo "     pass power7 bootstrap + FloatTest"
fi
if [ "$MODE" = be ]; then CPUS="power7 power9"; else CPUS="power8 power9"; fi
for cpu in $CPUS; do
	echo "==   QEMU_CPU=$cpu:"
	# The exception/unwind machinery is per-backend GEN code (ExceptionSignal,
	# BlockOnException, BlockUnwind + the outer-return unwind hook), so its
	# tests gate here too, not only on x64.
	for t in tests/FloatTest.st tests/FloatEdgeTest.st tests/FloatCrossRepTest.st \
	         tests/SmallFloat64BoundaryTest.st tests/FloatHashTest.st \
	         tests/ScaledDecimalTest.st tests/LargeIntegerTest.st \
	         tests/ExceptionTest.st tests/ExceptionProtocolTest.st \
	         tests/UnwindTest.st tests/ResumableTest.st; do
		QEMU_CPU=$cpu timeout 900 "$OUT/st" -s "$IMG" -f "$t" </dev/null >/dev/null 2>&1 \
			|| { echo "FAIL $t ($cpu)"; exit 1; }
		echo "     pass $(basename "$t")"
	done
done
echo "ALL $MODE .st TESTS PASSED under qemu"
