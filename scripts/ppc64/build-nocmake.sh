#!/usr/bin/env bash
# cmake-less build of the VM test surface, in two modes:
#
#   NATIVE (inside a foreign-arch container that lacks cmake):
#     scripts/ppc64/build-nocmake.sh <build-dir>
#
#   CROSS + STATIC (fast: native-speed compile in an x86_64 debian container
#   with gcc-powerpc64-linux-gnu, producing static BE binaries that run
#   directly on the host through binfmt/qemu-user-static):
#     TARGET_ARCH=ppc64 CC=powerpc64-linux-gnu-gcc STATIC=1 \
#       scripts/ppc64/build-nocmake.sh <build-dir>
#
# Static mode compiles everything into each executable (no libVM.so) so the
# emulated run needs no target sysroot.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:?usage: build-nocmake.sh <build-dir>}"
mkdir -p "$BUILD"
CC="${CC:-cc}"
TARGET_ARCH="${TARGET_ARCH:-$(uname -m)}"
STATIC="${STATIC:-0}"
CFLAGS="-std=gnu11 -O2 -fcommon -fno-omit-frame-pointer -I$ROOT/vm -I$ROOT"

# ONE backend serves both byte orders (vm/jit/ppc64/); the targets differ in
# the ABI bind (elfv1/elfv2), the CPU-floor bind (cpu/CpuBind*.c) and the
# goldens (byte vectors genuinely differ per byte order).
case "$TARGET_ARCH" in
	ppc64)
		ARCH_DIR="ppc64"
		ABI_SRCS="$ROOT/vm/jit/ppc64/abi/elfv1/AbiElfV1.c
			$ROOT/vm/jit/ppc64/abi/elfv1/AbiElfV1Bind.c
			$ROOT/vm/jit/ppc64/abi/elfv1/FiberElfV1.c
			$ROOT/vm/jit/ppc64/cpu/CpuBindBe.c"
		TEST_SRCS="$ROOT/vm/tests/EmitGoldenPpc64.c $ROOT/vm/tests/EmitGoldenPpc64Bind.c"
		CITYHASH_FLAGS="-DWORDS_BIGENDIAN"
		;;
	ppc64le)
		ARCH_DIR="ppc64"
		ABI_SRCS="$ROOT/vm/jit/ppc64/abi/elfv2/AbiElfV2.c
			$ROOT/vm/jit/ppc64/abi/elfv2/AbiElfV2Bind.c
			$ROOT/vm/jit/ppc64/abi/elfv2/FiberElfV2.c
			$ROOT/vm/jit/ppc64/cpu/CpuBindLe.c"
		TEST_SRCS="$ROOT/vm/tests/EmitGoldenPpc64le.c $ROOT/vm/tests/EmitGoldenPpc64leBind.c"
		CITYHASH_FLAGS=""
		;;
	*) echo "build-nocmake.sh targets ppc64/ppc64le (got $TARGET_ARCH)"; exit 1 ;;
esac

SOURCES=$(ls "$ROOT"/vm/compiler/*.c "$ROOT"/vm/concurrency/*.c "$ROOT"/vm/core/*.c \
	"$ROOT"/vm/jit/RegisterAllocator.c "$ROOT"/vm/jit/StubCode.c \
	"$ROOT"/vm/jit/$ARCH_DIR/*.c "$ROOT"/vm/memory/*.c "$ROOT"/vm/os/linux/*.c \
	"$ROOT"/vm/runtime/*.c "$ROOT"/vm/tools/*.c)
SOURCES="$SOURCES $ABI_SRCS $ROOT/vm/thirdparty/cityhash/city.c $ROOT/vm/thirdparty/linenoise/linenoise.c"

if [ "$STATIC" = "1" ]; then
	echo "cross-static build ($TARGET_ARCH via $CC)..."
	$CC $CFLAGS $CITYHASH_FLAGS -static -o "$BUILD/st" \
		"$ROOT/main.c" "$ROOT/vm/tests/SelfTests.c" $TEST_SRCS \
		$SOURCES -lpthread -lm
	$CC $CFLAGS $CITYHASH_FLAGS -static -o "$BUILD/TokenizerTest" \
		"$ROOT/vm/tests/TokenizerTest.c" $SOURCES -lpthread -lm
else
	echo "native shared build ($TARGET_ARCH)..."
	$CC $CFLAGS $CITYHASH_FLAGS -fPIC -shared -o "$BUILD/libVM.so" $SOURCES -lpthread -lm
	$CC $CFLAGS -o "$BUILD/st" "$ROOT/main.c" "$ROOT/vm/tests/SelfTests.c" \
		$TEST_SRCS -L"$BUILD" -lVM -Wl,-rpath,'$ORIGIN'
	$CC $CFLAGS -o "$BUILD/TokenizerTest" "$ROOT/vm/tests/TokenizerTest.c" \
		-L"$BUILD" -lVM -Wl,-rpath,'$ORIGIN'
fi
echo "build-nocmake done: $BUILD"
