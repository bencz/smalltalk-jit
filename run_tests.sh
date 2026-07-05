#!/usr/bin/env bash
#
# Runs the Smalltalk test suite.
#
#   ./run_tests.sh              build, bootstrap, run tests/*.st
#   ./run_tests.sh --all        also run every sample (samples + advanced + concurrency)
#   ./run_tests.sh --samples    run the samples instead of the tests
#   ./run_tests.sh --no-build   skip the build/bootstrap step (reuse build/ image)
#   BUILD=mybuild ./run_tests.sh   use a different build directory
#
# Each test is a script that raises on the first failed assertion, so a
# non-zero exit means failure. The runner prints a per-file result, re-shows
# the output of any failure, and exits non-zero if anything failed.

set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

BUILD="${BUILD:-build}"
SNAP="$BUILD/test-snapshot.img"
DO_BUILD=1
RUN_TESTS=1
RUN_SAMPLES=0

for arg in "$@"; do
	case "$arg" in
		--all)      RUN_SAMPLES=1 ;;
		--samples)  RUN_SAMPLES=1; RUN_TESTS=0 ;;
		--no-build) DO_BUILD=0 ;;
		*) echo "unknown option: $arg"; exit 2 ;;
	esac
done

# colours only on a terminal
if [ -t 1 ]; then
	G=$'\e[32m'; R=$'\e[31m'; B=$'\e[1m'; Z=$'\e[0m'
else
	G=""; R=""; B=""; Z=""
fi

if [ "$DO_BUILD" -eq 1 ]; then
	echo "${B}building...${Z}"
	cmake -S . -B "$BUILD" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null 2>&1
	if ! cmake --build "$BUILD" -j"$(nproc)" >/dev/null 2>&1; then
		echo "${R}BUILD FAILED${Z}"; exit 1
	fi
fi

export LD_LIBRARY_PATH="$BUILD"

if [ ! -x "$BUILD/st" ]; then
	echo "${R}no VM binary at $BUILD/st — build first (drop --no-build)${Z}"; exit 1
fi

# always bootstrap a fresh image so it matches the current kernel (cheap).
# stdin from /dev/null: bootstrapping falls through to the REPL, so feed it EOF
# to make it exit immediately instead of waiting for input.
echo "${B}bootstrapping image...${Z}"
if ! "$BUILD/st" -s "$SNAP" -b smalltalk </dev/null >/dev/null 2>&1; then
	echo "${R}BOOTSTRAP FAILED${Z}"; exit 1
fi

pass=0
fail=0
failed=""

run_group() {
	local title="$1"; shift
	echo ""
	echo "${B}${title}${Z}"
	local f base out
	for f in "$@"; do
		[ -f "$f" ] || continue
		base="$(basename "$f")"
		[ "$base" = "CompilerTestFile.st" ] && continue   # included by CompilerTest, not standalone
		[ "$base" = "06_business_card_server.st" ] && continue   # standalone server, runs forever
		out="$(timeout 120 "$BUILD/st" -s "$SNAP" -f "$f" </dev/null 2>&1)"
		if [ $? -eq 0 ]; then
			printf "  ${G}pass${Z}  %s\n" "$base"
			pass=$((pass + 1))
		else
			printf "  ${R}FAIL${Z}  %s\n" "$base"
			echo "$out" | sed 's/^/        /'
			fail=$((fail + 1))
			failed="$failed $base"
		fi
	done
}

[ "$RUN_TESTS" -eq 1 ] && run_group "tests" tests/*.st
if [ "$RUN_SAMPLES" -eq 1 ]; then
	run_group "samples" samples/*.st
	run_group "samples/advanced" samples/advanced/*.st
	run_group "samples/concurrency" samples/concurrency/*.st
fi

echo ""
echo "${B}================================${Z}"
if [ "$fail" -eq 0 ]; then
	echo "${G}${B}ALL PASSED${Z}  ($pass ok)"
	exit 0
else
	echo "${R}${B}$fail FAILED${Z} / $((pass + fail)) run:${R}$failed${Z}"
	exit 1
fi
