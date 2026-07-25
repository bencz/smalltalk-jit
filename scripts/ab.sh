#!/usr/bin/env bash
#
# Interleaved A/B benchmark runner.
#
#   scripts/ab.sh <benchA> <benchB> [rounds]
#
# where each bench arg is  LABEL=ENV_ASSIGNMENTS  and ENV_ASSIGNMENTS is a
# comma-separated list (or empty for "as built"):
#
#   scripts/ab.sh 'tier=' 'no-tier=ST_NO_TIER=1' 8
#   scripts/ab.sh 'base=' 'mixed=ST_NO_INLINE_MIXED=1'
#
# To compare two BUILDS rather than two configurations, point ST_AB_ROOT_<label>
# at another checkout (see scripts/report.awk for how to read the output):
#
#   ST_AB_ROOT_new=/path/to/tree scripts/ab.sh 'new=' 'old=' 8
#
# WHY THIS EXISTS, and why it does not just run A five times then B five times:
#
#  * Code LAYOUT is a lottery in this VM. Moving a cold block has produced a
#    reproducible 2 percent swing with identical executed instructions, and
#    changing loop-header alignment from 16 to 32 bytes moved a benchmark 4
#    percent the same way. A measurement that does not control for drift will
#    happily attribute a layout accident to the change under test.
#  * The machine warms up. Runs are therefore ABBA-paired: within each round
#    both configurations run twice in opposite order, so a monotonic thermal
#    drift cancels inside the round instead of biasing whichever side ran first.
#  * Wall clock is the WEAKEST signal available here. Every record therefore
#    also carries retired-instruction counts from perf when it is available;
#    those are deterministic to a few parts in 10^8 on this workload and will
#    tell you whether a change added work long before the clock will.
#
# Output is one JSONL record per run on stdout, ready to append to
# benchmarks/results/BASELINE.jsonl and to pipe through scripts/report.awk.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
	echo "usage: $0 <labelA=ENV,ENV> [labelB=ENV,ENV] [rounds]" >&2
	echo "       one spec records a BASELINE, two compare them interleaved" >&2
	exit 2
}

[ $# -ge 1 ] || usage
SPEC_A="$1"
LABEL_A="${SPEC_A%%=*}"
ENVS_A="${SPEC_A#*=}"

# One spec = baseline capture (no comparison to interleave against).
SOLO=0
if [ $# -ge 2 ] && [ "${2#*=}" != "$2" ]; then
	SPEC_B="$2"
	LABEL_B="${SPEC_B%%=*}"
	ENVS_B="${SPEC_B#*=}"
	ROUNDS="${3:-6}"
else
	SOLO=1
	LABEL_B=""
	ENVS_B=""
	ROUNDS="${2:-6}"
fi

BUILD="${BUILD:-build}"
# All six now that every one of them reports a batch total long enough to
# measure. They used to be three because the other three printed a constant
# (ArrayNumeric always "3 milliseconds", BigInt always "1"), so including them
# added runtime and no information.
BENCHES="${ST_AB_BENCHES:-Richards DeltaBlue FloatBench MixedArithBench ArrayNumericBench BigIntBench}"

# ---- machine hygiene -------------------------------------------------------
# Recorded in every JSONL line, not merely checked, so a result can be judged
# later without trusting that the machine was in the same state.
GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
NO_TURBO=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo unknown)
NPROC=$(nproc)
PIN_CPU="${ST_AB_CPU:-2}"
# Executions averaged into ONE recorded datum. Raising this cuts the noise of a
# datum by roughly sqrt(REPS) and costs proportionally more wall time; lowering
# it to 1 restores the old one-run-per-datum behaviour, which on this machine
# cannot resolve a 4% change. See the -r note in run_one.
REPS="${ST_AB_REPS:-15}"

if [ "$GOVERNOR" != "performance" ] && [ -z "${ST_AB_ALLOW_ANY_GOVERNOR:-}" ]; then
	echo "WARNING: cpufreq governor is '$GOVERNOR', not 'performance'." >&2
	echo "WARNING: frequency scaling will show up as benchmark noise." >&2
	echo "WARNING: set ST_AB_ALLOW_ANY_GOVERNOR=1 to proceed anyway." >&2
	exit 3
fi

HAVE_PERF=0
if command -v perf >/dev/null 2>&1 && perf stat -e instructions true >/dev/null 2>&1; then
	HAVE_PERF=1
fi

# ---- one run ---------------------------------------------------------------
# Wall clock in MICROseconds. EPOCHREALTIME is a bash builtin, so it costs no
# fork; the `date` arm is the fallback for a shell without it. Microseconds
# rather than milliseconds because the point of this rewrite is resolution: a
# 300 ms benchmark measured to the millisecond can only resolve 0.3%, and the
# changes worth detecting here are smaller than that.
now_us() {
	if [ -n "${EPOCHREALTIME:-}" ]; then
		local t="${EPOCHREALTIME/[.,]/}"
		echo $(( 10#$t ))
	else
		echo $(( $(date +%s%N) / 1000 ))
	fi
}


run_one() {
	local label="$1" envs="$2" bench="$3"
	# A label may contain anything readable (hyphens, dots); a shell variable
	# name may not, so map it before the indirect lookup.
	local safe="${label//[^A-Za-z0-9_]/_}"
	local root_var="ST_AB_ROOT_${safe}"
	local root="${!root_var:-$ROOT}"
	local img="$root/$BUILD/benchmark-snapshot.img"
	local envargs=()
	local ms insns out selfms t0 t1

	if [ -n "$envs" ]; then
		local IFS=','
		for kv in $envs; do envargs+=("$kv"); done
	fi

	if [ "$HAVE_PERF" -eq 1 ]; then
		# perf writes to stderr, the benchmark to stdout: keep them apart.
		#
		# Each DATUM is the MINIMUM over REPS executions, not one run and not
		# their mean.
		#
		# REPS, because a single run of Richards on this machine scatters over
		# about 30 ms (frequency scaling under a non-performance governor, plus
		# startup), which buries the effects worth detecting.
		#
		# MINIMUM rather than mean, because the noise here is one-sided:
		# interference can only make a run slower, never faster. Observed with
		# the mean: a label whose median was 674 ms had a p90 of 738 ms from a
		# couple of disturbed samples, and the sign test then refused a result
		# that the undisturbed runs agreed on. The minimum is the standard
		# estimator for "how fast does this go when nothing else interferes",
		# and it is exactly what we want to compare between two builds.
		local perfout msRun best
		perfout=$(mktemp)
		best=""
		local rep
		for ((rep = 0; rep < REPS; rep++)); do
			out=$(cd "$root" && env "${envargs[@]+"${envargs[@]}"}" \
				perf stat -x, -e instructions,task-clock -o "$perfout" \
				taskset -c "$PIN_CPU" ./"$BUILD"/st -s "$img" -f "benchmarks/$bench.st" \
				</dev/null 2>/dev/null | tail -1)
			msRun=$(awk -F, '$3 ~ /^task-clock/ {printf "%.3f", $1; exit}' "$perfout" 2>/dev/null)
			# Instructions are deterministic to about 8 significant digits here,
			# so any repetition's value is as good as another's; keep the one
			# that goes with the fastest run.
			if [ -n "$msRun" ] && { [ -z "$best" ] || awk -v a="$msRun" -v b="$best" 'BEGIN{exit !(a < b)}'; }; then
				best="$msRun"
				insns=$(awk -F, '$3 ~ /^instructions/ {print $1; exit}' "$perfout" 2>/dev/null)
			fi
		done
		ms="$best"
		rm -f "$perfout"
	else
		# No perf: time each of REPS runs from the shell, keep the fastest (same
		# one-sided-noise argument as the perf branch above).
		local repNoPerf best2 msRun2
		best2=""
		for ((repNoPerf = 0; repNoPerf < REPS; repNoPerf++)); do
			t0=$(now_us)
			out=$(cd "$root" && env "${envargs[@]+"${envargs[@]}"}" \
				taskset -c "$PIN_CPU" ./"$BUILD"/st -s "$img" -f "benchmarks/$bench.st" \
				</dev/null 2>/dev/null | tail -1)
			t1=$(now_us)
			msRun2=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", (b - a) / 1000}')
			if [ -z "$best2" ] || awk -v a="$msRun2" -v b="$best2" 'BEGIN{exit !(a < b)}'; then
				best2="$msRun2"
			fi
		done
		insns=""
		ms="$best2"
	fi

	# WALL NO LONGER COMES FROM THE BENCHMARK'S OWN PRINTOUT.
	#
	# It used to, and the printout was a truncated INTEGER mean: Richards
	# printed `total // 100` and so reported 6, 7 or 8 ms for every run in this
	# repository's entire recorded history while its instruction count moved by
	# percent. A 3% change could not be seen, and report.awk correctly but
	# uselessly answered INCONCLUSIVE every time. The self-reported figure is
	# still recorded as `selfms`, because it times only the benchmark's own
	# region and so brackets out startup, but it is no longer the wall signal.

	# The benchmark also self-verifies, so an unparseable line means the run
	# FAILED and must not be recorded as a datum.
	selfms=$(echo "$out" | grep -oE '[0-9]+ milliseconds' | grep -oE '^[0-9]+')
	if [ -z "$selfms" ]; then
		echo "FAILED $label $bench: $out" >&2
		return 1
	fi

	printf '{"label":"%s","bench":"%s","ms":%s,"selfms":%s,"insns":%s,"governor":"%s","no_turbo":"%s","nproc":%s,"cpu":%s,"env":"%s"}\n' \
		"$label" "$bench" "$ms" "$selfms" "${insns:-null}" "$GOVERNOR" "$NO_TURBO" "$NPROC" "$PIN_CPU" "$envs"
}

for bench in $BENCHES; do
	for ((i = 0; i < ROUNDS; i++)); do
		if [ "$SOLO" -eq 1 ]; then
			run_one "$LABEL_A" "$ENVS_A" "$bench" || exit 1
		else
			# ABBA, not AABB: the pair is symmetric about the middle of the round.
			run_one "$LABEL_A" "$ENVS_A" "$bench" || exit 1
			run_one "$LABEL_B" "$ENVS_B" "$bench" || exit 1
			run_one "$LABEL_B" "$ENVS_B" "$bench" || exit 1
			run_one "$LABEL_A" "$ENVS_A" "$bench" || exit 1
		fi
	done
done
