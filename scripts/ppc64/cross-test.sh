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
