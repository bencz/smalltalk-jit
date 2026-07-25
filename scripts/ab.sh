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
BENCHES="${ST_AB_BENCHES:-Richards DeltaBlue FloatBench}"

# ---- machine hygiene -------------------------------------------------------
# Recorded in every JSONL line, not merely checked, so a result can be judged
# later without trusting that the machine was in the same state.
GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
NO_TURBO=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo unknown)
NPROC=$(nproc)
PIN_CPU="${ST_AB_CPU:-2}"

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
run_one() {
	local label="$1" envs="$2" bench="$3"
	# A label may contain anything readable (hyphens, dots); a shell variable
	# name may not, so map it before the indirect lookup.
	local safe="${label//[^A-Za-z0-9_]/_}"
	local root_var="ST_AB_ROOT_${safe}"
	local root="${!root_var:-$ROOT}"
	local img="$root/$BUILD/benchmark-snapshot.img"
	local envargs=()
	local ms insns out

	if [ -n "$envs" ]; then
		local IFS=','
		for kv in $envs; do envargs+=("$kv"); done
	fi

	if [ "$HAVE_PERF" -eq 1 ]; then
		# perf writes to stderr, the benchmark to stdout: keep them apart.
		local perfout
		perfout=$(mktemp)
		out=$(cd "$root" && env "${envargs[@]+"${envargs[@]}"}" \
			perf stat -x, -e instructions -o "$perfout" \
			taskset -c "$PIN_CPU" ./"$BUILD"/st -s "$img" -f "benchmarks/$bench.st" \
			</dev/null 2>/dev/null | tail -1)
		insns=$(grep -E '^[0-9]+,' "$perfout" 2>/dev/null | head -1 | cut -d, -f1)
		rm -f "$perfout"
	else
		out=$(cd "$root" && env "${envargs[@]+"${envargs[@]}"}" \
			taskset -c "$PIN_CPU" ./"$BUILD"/st -s "$img" -f "benchmarks/$bench.st" \
			</dev/null 2>/dev/null | tail -1)
		insns=""
	fi

	# Every benchmark prints "<name> <N> milliseconds" and self-verifies, so an
	# unparseable line means the run FAILED and must not be recorded as a datum.
	ms=$(echo "$out" | grep -oE '[0-9]+ milliseconds' | grep -oE '^[0-9]+')
	if [ -z "$ms" ]; then
		echo "FAILED $label $bench: $out" >&2
		return 1
	fi

	printf '{"label":"%s","bench":"%s","ms":%s,"insns":%s,"governor":"%s","no_turbo":"%s","nproc":%s,"cpu":%s,"env":"%s"}\n' \
		"$label" "$bench" "$ms" "${insns:-null}" "$GOVERNOR" "$NO_TURBO" "$NPROC" "$PIN_CPU" "$envs"
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
