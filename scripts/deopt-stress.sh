#!/usr/bin/env bash
#
# Gate level 15: the INTERNAL ORACLE (ADR 0002).
#
# The dry cut took the external oracle off the branch, and the replacement is
# this: run the whole suite with the second code generator producing everything,
# and again with every speculation in it FAILING, and require the output to be
# IDENTICAL to tier 1's, file by file, byte for byte, exit code included.
#
# Three configurations, and each answers a different question:
#
#   baseline          tier 1 only. The reference.
#   ST_TIER2_ALL      tier 2 compiles and RUNS every method it can, speculations
#                     intact. Does the second code generator agree with the
#                     first, over the whole system rather than over the two dozen
#                     hand-written methods of gate level 7?
#   ST_DEOPT_STRESS   the same, and every speculation fails. Every optimized
#                     activation reconstructs a tier-1 frame and continues in it.
#                     Is leaving optimized code correct?
#
# WHY THE DEOPTIMIZATION COUNT IS CHECKED AND NOT JUST THE OUTPUT. Leaving
# optimized code is correct by construction, so a stress run in which nothing was
# ever optimized produces identical output and proves nothing at all. The count
# has to be non-zero or the pass is vacuous, and that is asserted here rather than
# assumed.
#
# ST_DEOPT_STRESS_SKIP walks the stress DEEPER: only the first guard a path
# reaches ever fires, because after it the rest of that activation belongs to
# tier 1, so one run stresses the first send site of every method. Skipping k
# sites moves it to the (k+1)-th.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD="${BUILD:-build}"
DEPTHS="${DEPTHS:-0 1 2}"

if [ -t 1 ]; then
	G=$'\e[32m'; R=$'\e[31m'; B=$'\e[1m'; D=$'\e[2m'; Z=$'\e[0m'
else
	G=""; R=""; B=""; D=""; Z=""
fi

SCRATCH="${TMPDIR:-/tmp}/st-stress-$$"
mkdir -p "$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT

IMAGE="$SCRATCH/core.img"
"$ROOT/$BUILD/st" -s "$IMAGE" -b packages/Core >/dev/null 2>&1 || {
	echo "${R}could not bootstrap an image${Z}"; exit 1; }

# One run of one file in one configuration. Answers its output plus its exit
# code, with the tier-2 report lines stripped: they are diagnostics about the
# RUN and differ between configurations by design, so comparing them would
# compare the instrument instead of the measurement.
run_one() {
	local file="$1" out status
	out="$(timeout 120 "$ROOT/$BUILD/st" -s "$IMAGE" -f "$file" 2>&1)"
	# CAPTURED HERE and not inside the printf below: the exit code of the run is
	# part of what has to be identical -- a test file answers its failure count as
	# its exit status -- and reading `$?` after anything else has run reads that
	# other thing's status instead.
	status=$?
	printf '%s\nexit=%d\n' "$(printf '%s\n' "$out" | grep -v '^tier2: ')" "$status"
}

FILES=$(ls tests/*.st 2>/dev/null)
total=0
differed=0
DIFFERING=""

for file in $FILES; do
	total=$((total + 1))
	base="$(run_one "$file")"

	for mode in tier2all stress; do
		if [ "$mode" = tier2all ]; then
			export ST_TIER2_ALL=1
			unset ST_DEOPT_STRESS ST_DEOPT_STRESS_SKIP 2>/dev/null || true
			depths="0"
		else
			unset ST_TIER2_ALL 2>/dev/null || true
			export ST_DEOPT_STRESS=1
			depths="$DEPTHS"
		fi
		for depth in $depths; do
			export ST_DEOPT_STRESS_SKIP="$depth"
			other="$(run_one "$file")"
			if [ "$base" != "$other" ]; then
				differed=$((differed + 1))
				DIFFERING="$DIFFERING $file[$mode:$depth]"
			fi
		done
		unset ST_DEOPT_STRESS_SKIP
	done
	unset ST_TIER2_ALL ST_DEOPT_STRESS 2>/dev/null || true
done

# The counts, from one representative run, so the pass is not vacuous.
report="$(ST_DEOPT_STRESS=1 timeout 300 "$ROOT/$BUILD/st" -s "$IMAGE" \
	-f tests/ArrayTest.st 2>&1 | grep '^tier2: stress' | tail -1)"
upgraded="$(printf '%s\n' "$report" | sed -n 's/.*upgraded=\([0-9]*\).*/\1/p')"
left="$(printf '%s\n' "$report" | sed -n 's/.*left=\([0-9]*\).*/\1/p')"

echo "${B}deopt-stress:${Z} $total files, $((differed)) differing"
echo "${D}  one run reports upgraded=${upgraded:-0} left=${left:-0}${Z}"

if [ -n "$DIFFERING" ]; then
	echo "${R}differing:${Z}$DIFFERING"
	exit 1
fi
if [ -z "${left:-}" ] || [ "${left:-0}" -eq 0 ]; then
	echo "${R}nothing ever left optimized code: the pass would be vacuous${Z}"
	exit 1
fi
if [ -z "${upgraded:-}" ] || [ "${upgraded:-0}" -eq 0 ]; then
	echo "${R}nothing was ever compiled by tier 2${Z}"
	exit 1
fi
echo "${G}identical in every configuration${Z}"
