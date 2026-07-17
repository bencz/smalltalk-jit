#!/usr/bin/env bash
#
# Runs the Smalltalk test suite.
#
#   ./run_tests.sh              build, bootstrap, run tests/*.st
#   ./run_tests.sh --all        also run every sample (samples + advanced + concurrency)
#                               AND the benchmark suite (self-verifying)
#   ./run_tests.sh --samples    run the samples instead of the tests
#   ./run_tests.sh --bench      also run the benchmark suite
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
RUN_BENCH=0

for arg in "$@"; do
	case "$arg" in
		--all)      RUN_SAMPLES=1; RUN_BENCH=1 ;;
		--samples)  RUN_SAMPLES=1; RUN_TESTS=0 ;;
		--bench)    RUN_BENCH=1 ;;
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

# C-level self-tests that gate every run (no image needed, milliseconds)
echo ""
echo "${B}self-tests${Z}"
for st in ST_SMALLFLOAT_TEST ST_ABI_EMIT_TEST; do
	if env "$st=1" "$BUILD/st" >/dev/null 2>&1; then
		printf "  ${G}pass${Z}  %s\n" "$st"
		pass=$((pass + 1))
	else
		printf "  ${R}FAIL${Z}  %s\n" "$st"
		fail=$((fail + 1))
		failed="$failed $st"
	fi
done

# The message-serializer self-test evals Smalltalk source, so unlike the loop
# above it needs the freshly bootstrapped image (-s).
if ST_MESSAGE_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1; then
	printf "  ${G}pass${Z}  %s\n" "ST_MESSAGE_TEST"
	pass=$((pass + 1))
else
	printf "  ${R}FAIL${Z}  %s\n" "ST_MESSAGE_TEST"
	fail=$((fail + 1))
	failed="$failed ST_MESSAGE_TEST"
fi

# Inline-cache stats self-test (needs the image): first run proves mono sites
# hit ~100% and poly sites take the counted fallback; the ST_NO_IC run proves
# the kill-switch zeroes the whole apparatus.
if ST_IC_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1; then
	printf "  ${G}pass${Z}  %s\n" "ST_IC_STATS_TEST"
	pass=$((pass + 1))
else
	printf "  ${R}FAIL${Z}  %s\n" "ST_IC_STATS_TEST"
	fail=$((fail + 1))
	failed="$failed ST_IC_STATS_TEST"
fi
if ST_NO_IC=1 ST_IC_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1; then
	printf "  ${G}pass${Z}  %s\n" "ST_IC_STATS_TEST(ST_NO_IC)"
	pass=$((pass + 1))
else
	printf "  ${R}FAIL${Z}  %s\n" "ST_IC_STATS_TEST(ST_NO_IC)"
	fail=$((fail + 1))
	failed="$failed ST_IC_STATS_TEST(ST_NO_IC)"
fi

# Tier stats self-test (needs the image): first run proves the hot-method
# recompile fires once and promoted guards carry the dispatches; the
# ST_NO_TIER run proves the kill-switch zeroes the whole apparatus.
if ST_TIER_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1; then
	printf "  ${G}pass${Z}  %s\n" "ST_TIER_STATS_TEST"
	pass=$((pass + 1))
else
	printf "  ${R}FAIL${Z}  %s\n" "ST_TIER_STATS_TEST"
	fail=$((fail + 1))
	failed="$failed ST_TIER_STATS_TEST"
fi
if ST_NO_TIER=1 ST_TIER_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1; then
	printf "  ${G}pass${Z}  %s\n" "ST_TIER_STATS_TEST(ST_NO_TIER)"
	pass=$((pass + 1))
else
	printf "  ${R}FAIL${Z}  %s\n" "ST_TIER_STATS_TEST(ST_NO_TIER)"
	fail=$((fail + 1))
	failed="$failed ST_TIER_STATS_TEST(ST_NO_TIER)"
fi
# Inline-off isolation: the tier without the M2 inliner must still promote
# (the M1 shape) and stay correct.
if ST_TIER_INLINE_MAX=0 ST_TIER_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1; then
	printf "  ${G}pass${Z}  %s\n" "ST_TIER_STATS_TEST(INLINE_MAX=0)"
	pass=$((pass + 1))
else
	printf "  ${R}FAIL${Z}  %s\n" "ST_TIER_STATS_TEST(INLINE_MAX=0)"
	fail=$((fail + 1))
	failed="$failed ST_TIER_STATS_TEST(INLINE_MAX=0)"
fi

run_group() {
	local title="$1"; shift
	echo ""
	echo "${B}${title}${Z}"
	local f base out
	for f in "$@"; do
		[ -f "$f" ] || continue
		base="$(basename "$f")"
		[ "$base" = "CompilerTestFile.st" ] && continue   # included by CompilerTest, not standalone
		[ "$base" = "IcHammerTest.st" ] && continue   # OS-thread stress: sandboxed group below
		[ "$base" = "TierHammerTest.st" ] && continue   # OS-thread stress: sandboxed group below
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

# The hammers drive Worker parallel: (real OS threads); project rule after
# the 2026-07-13 desktop freeze: run them PINNED inside a resource sandbox,
# never loose. Falls back to bare taskset (or a plain run) where systemd-run
# is unavailable.
run_sandboxed_hammer() {
	local f base out
	echo ""
	echo "${B}tests (sandboxed)${Z}"
	for f in tests/IcHammerTest.st tests/TierHammerTest.st; do
		base="$(basename "$f")"
		if command -v systemd-run >/dev/null 2>&1; then
			out="$(systemd-run --user --scope -q -p MemoryMax=6G -p TasksMax=300 \
				-- taskset -c 0-3 env LD_LIBRARY_PATH="$BUILD" \
				timeout 120 "$BUILD/st" -s "$SNAP" -f "$f" </dev/null 2>&1)"
		elif command -v taskset >/dev/null 2>&1; then
			out="$(taskset -c 0-3 env LD_LIBRARY_PATH="$BUILD" \
				timeout 120 "$BUILD/st" -s "$SNAP" -f "$f" </dev/null 2>&1)"
		else
			out="$(timeout 120 "$BUILD/st" -s "$SNAP" -f "$f" </dev/null 2>&1)"
		fi
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
[ "$RUN_TESTS" -eq 1 ] && run_sandboxed_hammer
if [ "$RUN_SAMPLES" -eq 1 ]; then
	run_group "samples" samples/*.st
	run_group "samples/advanced" samples/advanced/*.st
	run_group "samples/concurrency" samples/concurrency/*.st
fi

# Benchmarks self-verify their results (a wrong sum raises), so they are a
# correctness gate too, not just a stopwatch. The build was done above.
if [ "$RUN_BENCH" -eq 1 ]; then
	echo ""
	echo "${B}benchmarks${Z}"
	if BUILD="$BUILD" "$ROOT/run_benchmarks.sh" --no-build; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		failed="$failed benchmarks"
	fi
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
