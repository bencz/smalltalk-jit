#!/usr/bin/env bash
#
# Runs the benchmark suite (benchmarks/*.st).
#
#   ./run_benchmarks.sh              build, bootstrap, run every benchmark
#   ./run_benchmarks.sh --no-build   skip the build/bootstrap step (reuse build/ image)
#   ./run_benchmarks.sh --compare    also run with ST_NO_INLINE_CF=1, to show what
#                                    the JIT's inlined control flow is worth
#   BUILD=mybuild ./run_benchmarks.sh   use a different build directory
#
# WARNING: benchmarks/ is third-party GPL v2 code, unlike the rest of this
# BSD-licensed repository. See benchmarks/README.md.
#
# Each benchmark verifies its own result and raises if the computation is wrong,
# so a printed figure also means the run was correct and a non-zero exit means
# failure.

set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

BUILD="${BUILD:-build}"
SNAP="$BUILD/benchmark-snapshot.img"
DO_BUILD=1
COMPARE=0

for arg in "$@"; do
	case "$arg" in
		--no-build) DO_BUILD=0 ;;
		--compare)  COMPARE=1 ;;
		*) echo "unknown option: $arg"; exit 2 ;;
	esac
done

# colours only on a terminal
if [ -t 1 ]; then
	G=$'\e[32m'; R=$'\e[31m'; B=$'\e[1m'; D=$'\e[2m'; Z=$'\e[0m'
else
	G=""; R=""; B=""; D=""; Z=""
fi

if [ "$DO_BUILD" -eq 1 ]; then
	echo "${B}building...${Z}"
	cmake -S . -B "$BUILD" >/dev/null 2>&1
	if ! cmake --build "$BUILD" -j"$(nproc)" >/dev/null 2>&1; then
		echo "${R}BUILD FAILED${Z}"; exit 1
	fi
fi

export LD_LIBRARY_PATH="$BUILD"

if [ ! -x "$BUILD/st" ]; then
	echo "${R}no VM binary at $BUILD/st, build first (drop --no-build)${Z}"; exit 1
fi

# always bootstrap a fresh image so it matches the current kernel (cheap)
if ! "$BUILD/st" -s "$SNAP" -b smalltalk >/dev/null 2>&1; then
	echo "${R}BOOTSTRAP FAILED${Z}"; exit 1
fi

fail=0
failed=""

for f in benchmarks/*.st; do
	name="$(basename "$f")"
	printf "  %-16s " "$name"
	# stdbuf: the VM's assertion path aborts, which would discard a buffered message
	out="$(stdbuf -o0 "$BUILD/st" -s "$SNAP" -f "$f" 2>&1)"
	if [ $? -ne 0 ] || [ -z "$out" ]; then
		echo "${R}FAILED${Z}"
		[ -n "$out" ] && echo "$out" | sed 's/^/      /'
		fail=$((fail + 1)); failed="$failed $name"
		continue
	fi
	echo "${G}$out${Z}"

	if [ "$COMPARE" -eq 1 ]; then
		slow="$(ST_NO_INLINE_CF=1 stdbuf -o0 "$BUILD/st" -s "$SNAP" -f "$f" 2>&1)"
		printf "  %-16s ${D}%s   (ST_NO_INLINE_CF=1)${Z}\n" "" "$slow"
	fi
done

echo ""
echo "${B}================================${Z}"
if [ "$fail" -eq 0 ]; then
	echo "${G}${B}ALL RAN${Z}"
	exit 0
else
	echo "${R}${B}$fail FAILED${Z}:${R}$failed${Z}"
	exit 1
fi
