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
if ! "$BUILD/st" -s "$SNAP" -b packages/Core </dev/null >/dev/null 2>&1; then
	echo "${R}BOOTSTRAP FAILED${Z}"; exit 1
fi

# The samples project image: core + the Std.* packages the samples use
# (samples/package.st). Samples run against it through the harness (explicit
# ST_IMAGE/-s is the debug/override surface st run uses implicitly). Fresh
# core.img is always newer than the cached project image, so this rebuilds
# every run (cheap, and the point of the gate).
SNAP_ABS="$(cd "$(dirname "$SNAP")" && pwd)/$(basename "$SNAP")"
ST_ABS="$(cd "$BUILD" && pwd)/st"
DEVSNAP="$ROOT/samples/.stbuild/program.img"
echo "${B}building samples project image (core + Std packages)...${Z}"
if ! (cd samples && ST_PACKAGE_PATH="$ROOT/packages" ST_IMAGE="$SNAP_ABS" \
		"$ST_ABS" build >/dev/null 2>&1); then
	echo "${R}SAMPLES IMAGE BUILD FAILED${Z}"
	(cd samples && ST_PACKAGE_PATH="$ROOT/packages" ST_IMAGE="$SNAP_ABS" "$ST_ABS" build)
	exit 1
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

# Gate-of-the-gate: an UNCAUGHT Smalltalk error must exit nonzero (the VM
# counts fiber deaths in Exception>>defaultAction and main folds them into the
# exit code). If this ever exits 0 again, every assertion-style test in the
# suite can silently false-pass, so the suite itself must go red.
if "$BUILD/st" -s "$SNAP" -e 'nil zork' </dev/null >/dev/null 2>&1; then
	printf "  ${R}FAIL${Z}  %s\n" "UNCAUGHT_ERROR_EXITS_NONZERO"
	fail=$((fail + 1))
	failed="$failed UNCAUGHT_ERROR_EXITS_NONZERO"
else
	printf "  ${G}pass${Z}  %s\n" "UNCAUGHT_ERROR_EXITS_NONZERO"
	pass=$((pass + 1))
fi

# Project tooling e2e gate: the whole st new/build/run/test flow against the
# fresh image. Covers scaffold, build, the up-to-date fast path, staleness on
# a touched source, entry-point exit-code propagation, requires-by-name
# resolution through ST_PACKAGE_PATH, and st test fail counts.
project_e2e() {
	# the subshells cd around, so every path here must be absolute
	local dir SNAP ST
	SNAP="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
	ST="$(cd "$BUILD" && pwd)/st"
	dir="$(cd "$BUILD" && pwd)/e2e-project"
	rm -rf "$dir"
	mkdir -p "$dir/roots/E2e.Lib/src"
	printf 'PackageSpec new\n\tname: %s;\n\tfiles: #(%s);\n\tyourself\n' \
		"'E2e.Lib'" "'src/Lib.st'" > "$dir/roots/E2e.Lib/package.st"
	printf 'E2eGreeter := Object [\n\tclass greet [ ^%s ]\n]\n' "'lib says hi'" \
		> "$dir/roots/E2e.Lib/src/Lib.st"
	( cd "$dir" \
		&& ST_IMAGE="$SNAP" "$ST" new hello >/dev/null 2>&1 \
		&& cd hello \
		&& ST_IMAGE="$SNAP" "$ST" build 2>&1 | grep -q "^built " \
		&& ST_IMAGE="$SNAP" "$ST" build 2>&1 | grep -q "up to date" \
		&& ST_IMAGE="$SNAP" "$ST" run 2>&1 | grep -q "hello from hello" \
		&& ST_IMAGE="$SNAP" "$ST" test >/dev/null 2>&1 \
		&& sed -i "s/\\^0/^7/" src/Main.st \
		&& ST_IMAGE="$SNAP" "$ST" run >/dev/null 2>&1; [ $? -eq 7 ] ) || return 1
	( cd "$dir/hello" \
		&& sed -i "s/entry: #Main;/requires: 'E2e.Lib'; entry: #Main;/" package.st \
		&& sed -i "s/\\^7/Transcript nextPutAll: E2eGreeter greet; lf. ^0/" src/Main.st \
		&& ST_PACKAGE_PATH="$dir/roots" ST_IMAGE="$SNAP" "$ST" run 2>&1 | grep -q "lib says hi" \
		&& sed -i "s/that: \\[true\\]/that: [false]/" tests/MainTest.st \
		&& ST_PACKAGE_PATH="$dir/roots" ST_IMAGE="$SNAP" "$ST" test >/dev/null 2>&1; [ $? -eq 1 ] ) || return 1
	return 0
}
if project_e2e "$SNAP"; then
	printf "  ${G}pass${Z}  %s\n" "PROJECT_TOOLING_E2E"
	pass=$((pass + 1))
else
	printf "  ${R}FAIL${Z}  %s\n" "PROJECT_TOOLING_E2E"
	fail=$((fail + 1))
	failed="$failed PROJECT_TOOLING_E2E"
fi

# The committed project samples must keep building and answering what
# samples/projects/README.md promises: declared namespaces plus the
# reflective listing (namespaces/), package modules with imports and
# first-import-wins shadowing (modules/), and the minimal app (hello/).
project_samples() {
	rm -rf "$ROOT"/samples/projects/*/.stbuild "$ROOT"/samples/projects/modules/*/.stbuild
	( cd "$ROOT/samples/projects/namespaces" \
		&& ST_IMAGE="$SNAP_ABS" "$ST_ABS" run 2>&1 | grep -q "class format:" \
		&& ST_IMAGE="$SNAP_ABS" "$ST_ABS" test >/dev/null 2>&1 ) || return 1
	( cd "$ROOT/samples/projects/modules/app" \
		&& ST_IMAGE="$SNAP_ABS" "$ST_ABS" run 2>&1 | grep -q "Plain.Formatter" \
		&& ST_IMAGE="$SNAP_ABS" "$ST_ABS" test >/dev/null 2>&1 ) || return 1
	( cd "$ROOT/samples/projects/hello" \
		&& ST_IMAGE="$SNAP_ABS" "$ST_ABS" run 2>&1 | grep -q "hello from a project" ) || return 1
	( cd "$ROOT/samples/projects/store" \
		&& ST_IMAGE="$SNAP_ABS" "$ST_ABS" run 2>&1 | grep -q "totals agree" \
		&& ST_IMAGE="$SNAP_ABS" "$ST_ABS" test >/dev/null 2>&1 ) || return 1
	return 0
}
if project_samples; then
	printf "  ${G}pass${Z}  %s\n" "PROJECT_SAMPLES"
	pass=$((pass + 1))
else
	printf "  ${R}FAIL${Z}  %s\n" "PROJECT_SAMPLES"
	fail=$((fail + 1))
	failed="$failed PROJECT_SAMPLES"
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

# run_group <title> <image> <files...>: each file through the -f path against
# the given image (core tests run on the core image; samples run on the
# samples project image so unqualified lib references resolve).
run_group() {
	local title="$1"; shift
	local image="$1"; shift
	echo ""
	echo "${B}${title}${Z}"
	local f base out
	for f in "$@"; do
		[ -f "$f" ] || continue
		base="$(basename "$f")"
		[ "$base" = "package.st" ] && continue   # a project manifest, not a runnable script
		[ "$base" = "CompilerTestFile.st" ] && continue   # included by CompilerTest, not standalone
		[ "$base" = "IcHammerTest.st" ] && continue   # OS-thread stress: sandboxed group below
		[ "$base" = "TierHammerTest.st" ] && continue   # OS-thread stress: sandboxed group below
		[ "$base" = "AtomicStressTest.st" ] && continue   # OS-thread stress: sandboxed group below
		[ "$base" = "ExtendHammerTest.st" ] && continue   # OS-thread stress: sandboxed group below
		[ "$base" = "06_business_card_server.st" ] && continue   # standalone server, runs forever
		out="$(timeout 120 "$BUILD/st" -s "$image" -f "$f" </dev/null 2>&1)"
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

# Every shipped package's own tests, through the real st test flow (build the
# lib if stale, compile its testFiles into <Name>Tests importing the package
# and its direct deps, sum the fail counts).
run_package_tests() {
	echo ""
	echo "${B}package tests (st test)${Z}"
	local p name out
	for p in packages/*/; do
		[ -f "$p/package.st" ] || continue
		name="$(basename "$p")"
		out="$( (cd "$p" && ST_PACKAGE_PATH="$ROOT/packages" ST_IMAGE="$SNAP_ABS" \
			timeout 300 "$ST_ABS" test) </dev/null 2>&1 )"
		if [ $? -eq 0 ]; then
			printf "  ${G}pass${Z}  %s\n" "$name"
			pass=$((pass + 1))
		else
			printf "  ${R}FAIL${Z}  %s\n" "$name"
			echo "$out" | sed 's/^/        /'
			fail=$((fail + 1))
			failed="$failed $name"
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
	for f in tests/IcHammerTest.st tests/TierHammerTest.st tests/AtomicStressTest.st tests/ExtendHammerTest.st; do
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

[ "$RUN_TESTS" -eq 1 ] && run_group "tests" "$SNAP" tests/*.st
[ "$RUN_TESTS" -eq 1 ] && run_package_tests
[ "$RUN_TESTS" -eq 1 ] && run_sandboxed_hammer
if [ "$RUN_SAMPLES" -eq 1 ]; then
	run_group "samples" "$DEVSNAP" samples/*.st
	run_group "samples/advanced" "$DEVSNAP" samples/advanced/*.st
	run_group "samples/concurrency" "$DEVSNAP" samples/concurrency/*.st
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
