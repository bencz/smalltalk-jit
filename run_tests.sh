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
# non-zero exit means failure. The runner prints a per-file result WITH ITS
# WALL TIME, re-shows the output of any failure, and exits non-zero if anything
# failed.
#
# Timing knobs:
#   SLOW_MS=500    flag any item at or above this many ms (0 disables the flag)
#   SLOWEST_N=8    how many entries the closing "slowest" list shows (0 hides it)

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
	G=$'\e[32m'; R=$'\e[31m'; B=$'\e[1m'; Y=$'\e[33m'; D=$'\e[2m'; Z=$'\e[0m'
else
	G=""; R=""; B=""; Y=""; D=""; Z=""
fi

# ---- per-item timing -------------------------------------------------------
# Every item is timed and its wall time printed. The point is not curiosity: a
# suite of 150 items that each take 40 ms is dominated by any single one that
# takes seconds, and in a flat list of passes that item is invisible. Anything
# at or above SLOW_MS is flagged inline, and the slowest few are repeated at the
# end so the outlier cannot be missed.
SLOW_MS="${SLOW_MS:-500}"
SLOWEST_N="${SLOWEST_N:-8}"
TIMINGS=""

# Milliseconds. EPOCHREALTIME (bash 5) avoids forking a `date` per measurement,
# which across a suite this size would itself be a measurable share of the
# total. The character class covers locales that render the separator as a
# comma; 10# forces base 10 so a leading zero is not read as octal.
if [ -n "${EPOCHREALTIME:-}" ]; then
	now_ms() { local t="${EPOCHREALTIME/[.,]/}"; echo $(( 10#$t / 1000 )); }
else
	now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
fi

# One result line: status, name, wall time, and a flag when it is slow enough
# to distort the run. Callers print any failure output themselves.
report_result() {
	local rc="$1" name="$2" ms="$3" mark=""
	[ "$ms" -ge "$SLOW_MS" ] && mark="  ${Y}slow${Z}"
	TIMINGS="$TIMINGS$ms $name"$'\n'
	if [ "$rc" -eq 0 ]; then
		printf "  ${G}pass${Z}  %-44s ${D}%6s ms${Z}%s\n" "$name" "$ms" "$mark"
		pass=$((pass + 1))
	else
		printf "  ${R}FAIL${Z}  %-44s ${D}%6s ms${Z}\n" "$name" "$ms"
		fail=$((fail + 1))
		failed="$failed $name"
	fi
}

SUITE_T0=$(now_ms)

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
# A FAILURE HERE IS REPORTED, NOT FATAL TO THE RUN.
#
# It used to `exit 1`, and under the dry cut that made the runner useless: one
# missing subsystem stopped the suite before a single test file had run, so the
# whole thing said nothing about the other 154 items. A runner that cannot
# report the state of what DOES work cannot be used to steer.
#
# It stays fatal to the EXIT CODE -- SAMPLES_IMAGE is counted as a failed item
# below, so "158 ok" still means what it meant and the level 13 criterion is
# unchanged. What changes is only that the rest of the suite gets to run and be
# counted first. The sample GROUP is skipped when this fails, because running
# samples against an image that was never built would report 71 failures that
# all have one cause.
SAMPLES_IMAGE_OK=1
if ! (cd samples && ST_PACKAGE_PATH="$ROOT/packages" ST_IMAGE="$SNAP_ABS" \
		"$ST_ABS" build >/dev/null 2>&1); then
	SAMPLES_IMAGE_OK=0
	echo "${R}SAMPLES IMAGE BUILD FAILED${Z} — the sample groups will be skipped"
	(cd samples && ST_PACKAGE_PATH="$ROOT/packages" ST_IMAGE="$SNAP_ABS" "$ST_ABS" build) \
		2>&1 | sed 's/^/        /'
fi

pass=0
fail=0
failed=""

# Counted as an ordinary item so it lands in the totals like everything else.
# This is what keeps the change above from being a way to hide a red suite.
report_result "$((1 - SAMPLES_IMAGE_OK))" "SAMPLES_IMAGE" 0

# C-level self-tests that gate every run (no image needed, milliseconds)
echo ""
echo "${B}self-tests${Z}"
for st in ST_SMALLFLOAT_TEST ST_BIGINT_TEST ST_ABI_EMIT_TEST; do
	t0=$(now_ms)
	env "$st=1" "$BUILD/st" >/dev/null 2>&1
	rc=$?
	report_result "$rc" "$st" $(( $(now_ms) - t0 ))
done

# The message-serializer self-test evals Smalltalk source, so unlike the loop
# above it needs the freshly bootstrapped image (-s).
t0=$(now_ms)
ST_MESSAGE_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1
rc=$?
report_result "$rc" "ST_MESSAGE_TEST" $(( $(now_ms) - t0 ))

# Inline-cache stats self-test (needs the image): first run proves mono sites
# hit ~100% and poly sites take the counted fallback; the ST_NO_IC run proves
# the kill-switch zeroes the whole apparatus.
t0=$(now_ms)
ST_IC_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1
rc=$?
report_result "$rc" "ST_IC_STATS_TEST" $(( $(now_ms) - t0 ))
t0=$(now_ms)
ST_NO_IC=1 ST_IC_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1
rc=$?
report_result "$rc" "ST_IC_STATS_TEST(ST_NO_IC)" $(( $(now_ms) - t0 ))

# Gate-of-the-gate: an UNCAUGHT Smalltalk error must exit nonzero (the VM
# counts fiber deaths in Exception>>defaultAction and main folds them into the
# exit code). If this ever exits 0 again, every assertion-style test in the
# suite can silently false-pass, so the suite itself must go red.
t0=$(now_ms)
"$BUILD/st" -s "$SNAP" -e 'nil zork' </dev/null >/dev/null 2>&1
# INVERTED: exiting 0 here is the failure.
[ $? -ne 0 ]; rc=$?
report_result "$rc" "UNCAUGHT_ERROR_EXITS_NONZERO" $(( $(now_ms) - t0 ))

# And it must STOP. Exception>>defaultAction ends in `Processor thisProcess
# terminate`, and for the main process that means the program is over: the
# statement after the error must not run. Exiting nonzero is not enough on its
# own -- the count made the status nonzero while execution carried on, so the
# signal expression answered the Error object and every send after it went to
# the wrong receiver. That is the doesNotUnderstand cascade this checks against.
#
# It CANNOT be an ordinary test file: the process it is about is the one running
# the test, and a file that proves this dies before it can report.
t0=$(now_ms)
out="$("$BUILD/st" -s "$SNAP" -e "nil zork. 'REACHED' printNl" </dev/null 2>&1)"
case "$out" in *REACHED*) rc=1 ;; *) rc=0 ;; esac
report_result "$rc" "TERMINATE_STOPS_THE_PROCESS" $(( $(now_ms) - t0 ))

# And the pending cleanups on the way out must run, because
# Block>>valueUnwindProtected: promises they do when "Process terminate" cuts
# through the frame.
#
# ifCurtailed: AND NOT ensure:, and the difference is the whole check. ensure:
# runs its block itself on the NORMAL path, so it prints either way and proves
# nothing -- measured against the pre-fix VM, which printed it. ifCurtailed:
# runs ONLY during an unwind, so it prints exactly when a terminate really
# unwound through here. Same reason as above for it not being a test file: the
# process being unwound is this one.
t0=$(now_ms)
out="$("$BUILD/st" -s "$SNAP" -e "[nil zork] ifCurtailed: ['CURTAILED' printNl]" \
	</dev/null 2>&1)"
case "$out" in *CURTAILED*) rc=0 ;; *) rc=1 ;; esac
report_result "$rc" "TERMINATE_RUNS_PENDING_CLEANUPS" $(( $(now_ms) - t0 ))

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
t0=$(now_ms)
project_e2e "$SNAP"
rc=$?
report_result "$rc" "PROJECT_TOOLING_E2E" $(( $(now_ms) - t0 ))

# FloatArray's byte payload must survive a SNAPSHOT unchanged. That cannot be
# asserted from a test running in an already-loaded image, so it is done
# through the project tooling: the array is built in a class-side initialize,
# which runs during `st build` and is therefore serialized, and `st run` loads
# that image and reads it back. Covers the values that make the encoding
# interesting: one that must box on read, an infinity, and one whose decimal
# spelling is not its exact value.
floatarray_snapshot_e2e() {
	local dir SNAP ST
	SNAP="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
	ST="$(cd "$BUILD" && pwd)/st"
	dir="$(cd "$BUILD" && pwd)/e2e-floatarray"
	rm -rf "$dir"
	mkdir -p "$dir"
	( cd "$dir" && ST_IMAGE="$SNAP" "$ST" new fa >/dev/null 2>&1 ) || return 1
	cat > "$dir/fa/src/Main.st" <<'FAEOF'
FaHolder := Object [
	| Samples |
	class initialize [
		Samples := FloatArray new: 4.
		Samples at: 1 put: 1.5.
		Samples at: 2 put: 1.0e300.
		Samples at: 3 put: 0.1.
		Samples at: 4 put: Float infinity.
	]
	class samples [ ^Samples ]
]

Main := Object [
	class main: args [
		| a bad |
		a := FaHolder samples.
		bad := 0.
		a size = 4 ifFalse: [bad := bad + 1].
		(a at: 1) = 1.5 ifFalse: [bad := bad + 1].
		(a at: 2) = 1.0e300 ifFalse: [bad := bad + 1].
		(a at: 2) class = BoxedFloat64 ifFalse: [bad := bad + 1].
		(a at: 3) = 0.1 ifFalse: [bad := bad + 1].
		((a at: 3) asExactFraction = (1/10)) ifTrue: [bad := bad + 1].
		(a at: 4) isInfinite ifFalse: [bad := bad + 1].
		a sum isInfinite ifFalse: [bad := bad + 1].
		^bad
	]
]
FAEOF
	( cd "$dir/fa" \
		&& ST_IMAGE="$SNAP" "$ST" build 2>&1 | grep -q "^built " \
		&& ST_IMAGE="$SNAP" "$ST" run >/dev/null 2>&1; [ $? -eq 0 ] ) || return 1
	return 0
}
t0=$(now_ms)
floatarray_snapshot_e2e "$SNAP"
rc=$?
report_result "$rc" "FLOATARRAY_SNAPSHOT" $(( $(now_ms) - t0 ))

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
t0=$(now_ms)
project_samples
rc=$?
report_result "$rc" "PROJECT_SAMPLES" $(( $(now_ms) - t0 ))

# Tier stats self-test (needs the image): first run proves the hot-method
# recompile fires once and promoted guards carry the dispatches; the
# ST_NO_TIER run proves the kill-switch zeroes the whole apparatus.
t0=$(now_ms)
ST_TIER_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1
rc=$?
report_result "$rc" "ST_TIER_STATS_TEST" $(( $(now_ms) - t0 ))
t0=$(now_ms)
ST_NO_TIER=1 ST_TIER_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1
rc=$?
report_result "$rc" "ST_TIER_STATS_TEST(ST_NO_TIER)" $(( $(now_ms) - t0 ))
# Inline-off isolation: the tier without the M2 inliner must still promote
# (the M1 shape) and stay correct.
t0=$(now_ms)
ST_TIER_INLINE_MAX=0 ST_TIER_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1
rc=$?
report_result "$rc" "ST_TIER_STATS_TEST(INLINE_MAX=0)" $(( $(now_ms) - t0 ))
# The boolean class-check fast path compiles a DIFFERENT shape for every
# inlined conditional in the image, so the tier apparatus has to be proven in
# both shapes: with it off, every check goes back through generateLoadClass.
t0=$(now_ms)
ST_NO_FUSE_BOOL=1 ST_TIER_STATS_TEST=1 "$BUILD/st" -s "$SNAP" </dev/null >/dev/null 2>&1
rc=$?
report_result "$rc" "ST_TIER_STATS_TEST(NO_FUSE_BOOL)" $(( $(now_ms) - t0 ))

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
		t0=$(now_ms)
		out="$(timeout 120 "$BUILD/st" -s "$image" -f "$f" </dev/null 2>&1)"
		rc=$?
		report_result "$rc" "$base" $(( $(now_ms) - t0 ))
		[ "$rc" -eq 0 ] || echo "$out" | sed 's/^/        /'
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
		t0=$(now_ms)
		out="$( (cd "$p" && ST_PACKAGE_PATH="$ROOT/packages" ST_IMAGE="$SNAP_ABS" \
			timeout 300 "$ST_ABS" test) </dev/null 2>&1 )"
		rc=$?
		report_result "$rc" "$name" $(( $(now_ms) - t0 ))
		[ "$rc" -eq 0 ] || echo "$out" | sed 's/^/        /'
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
		t0=$(now_ms)
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
		rc=$?
		report_result "$rc" "$base" $(( $(now_ms) - t0 ))
		[ "$rc" -eq 0 ] || echo "$out" | sed 's/^/        /'
	done
}

[ "$RUN_TESTS" -eq 1 ] && run_group "tests" "$SNAP" tests/*.st
[ "$RUN_TESTS" -eq 1 ] && run_package_tests
[ "$RUN_TESTS" -eq 1 ] && run_sandboxed_hammer
if [ "$RUN_SAMPLES" -eq 1 ] && [ "$SAMPLES_IMAGE_OK" -eq 0 ]; then
	echo ""
	echo "${Y}skipping the sample groups: the samples project image did not build${Z}"
	echo "${D}  (counted once as SAMPLES_IMAGE above; running them here would report${Z}"
	echo "${D}   71 failures with a single cause)${Z}"
fi
if [ "$RUN_SAMPLES" -eq 1 ] && [ "$SAMPLES_IMAGE_OK" -eq 1 ]; then
	run_group "samples" "$DEVSNAP" samples/*.st
	run_group "samples/advanced" "$DEVSNAP" samples/advanced/*.st
	run_group "samples/concurrency" "$DEVSNAP" samples/concurrency/*.st
fi

# Benchmarks self-verify their results (a wrong sum raises), so they are a
# correctness gate too, not just a stopwatch. The build was done above.
if [ "$RUN_BENCH" -eq 1 ]; then
	echo ""
	echo "${B}benchmarks${Z}"
	t0=$(now_ms)
	BUILD="$BUILD" "$ROOT/run_benchmarks.sh" --no-build
	rc=$?
	report_result "$rc" "benchmarks" $(( $(now_ms) - t0 ))
fi

# Slowest items. A single pathological entry can cost more than the other 150
# combined and is invisible in a list of passes; this is the line that makes it
# obvious. Set SLOWEST_N=0 to suppress.
if [ "$SLOWEST_N" -gt 0 ]; then
	echo ""
	echo "${B}slowest $SLOWEST_N${Z}"
	printf '%s' "$TIMINGS" | sort -rn | head -n "$SLOWEST_N" \
		| while read -r ms name; do
			mark=""
			[ "$ms" -ge "$SLOW_MS" ] && mark="  ${Y}slow${Z}"
			printf "  %-44s ${D}%6s ms${Z}%s\n" "$name" "$ms" "$mark"
		done
fi

echo ""
echo "${B}================================${Z}"
printf "total %s ms\n" "$(( $(now_ms) - SUITE_T0 ))"
if [ "$fail" -eq 0 ]; then
	echo "${G}${B}ALL PASSED${Z}  ($pass ok)"
	exit 0
else
	echo "${R}${B}$fail FAILED${Z} / $((pass + fail)) run:${R}$failed${Z}"
	exit 1
fi
