#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Detects the number of processors in a more portable way.
if command -v nproc >/dev/null 2>&1; then
    JOBS=$(nproc)
elif command -v getconf >/dev/null 2>&1; then
    JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
else
    JOBS=1
fi

# Basic safety check before running rm -rf.
case "$BUILD_DIR" in
    ""|"/")
        printf 'Invalid build directory: %s\n' "$BUILD_DIR" >&2
        exit 1
        ;;
esac

printf 'Configuring build in: %s\n' "$BUILD_DIR"
printf 'Build type: %s\n' "$BUILD_TYPE"
printf 'Parallel compilation: %s processes\n' "$JOBS"

# Wall-clock phases. This is a from-scratch build (the rm below) plus the whole
# gate, so it is the longest thing anyone runs here; knowing which half the time
# went to is the difference between "the compiler is slow" and "a test is slow".
#
# MILLISECONDS, not seconds: with ccache warm the compile is genuinely
# sub-second, and a whole-number seconds display just reads "0" and looks
# broken. `date` rather than bash's EPOCHREALTIME because this script is
# /bin/sh; the fallback keeps it working where %N is unsupported.
now_ms() {
    n=$(date +%s%N 2>/dev/null) || n=""
    case "$n" in
        *N|"") echo $(( $(date +%s) * 1000 )) ;;
        *)     echo $(( n / 1000000 )) ;;
    esac
}
phase_start=$(now_ms)
total_start=$phase_start

rm -rf -- "$BUILD_DIR"

cmake \
    -S "$SCRIPT_DIR" \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

cmake \
    --build "$BUILD_DIR" \
    --parallel "$JOBS"

build_end=$(now_ms)
printf '\nbuild: %s ms\n' "$((build_end - phase_start))"

# --no-build: this script just built it, and run_tests.sh would otherwise
# reconfigure and rebuild the same tree a second time.
# `|| rc=$?` rather than a bare call: set -e would abort here on a failing gate
# and the timing summary below would never print, which is exactly when it is
# most wanted.
rc=0
"$SCRIPT_DIR/run_tests.sh" --all --no-build || rc=$?

test_end=$(now_ms)
printf '\nbuild %s ms + tests %s ms = %s ms total\n' \
    "$((build_end - phase_start))" \
    "$((test_end - build_end))" \
    "$((test_end - total_start))"
exit $rc