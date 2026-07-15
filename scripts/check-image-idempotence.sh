#!/usr/bin/env bash
# Image-format idempotence: loading an image and re-saving it must be a
# fixpoint. The FIRST resave normalizes bootstrap-time transients; the second
# must reproduce it byte for byte.
#
#   BUILD=build ./scripts/check-image-idempotence.sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BUILD="${BUILD:-build}"
export LD_LIBRARY_PATH="$BUILD"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

"$BUILD/st" -s "$TMP/img1" -b smalltalk </dev/null >/dev/null
ST_RESAVE="$TMP/img2" "$BUILD/st" -s "$TMP/img1" </dev/null >/dev/null
ST_RESAVE="$TMP/img3" "$BUILD/st" -s "$TMP/img2" </dev/null >/dev/null

if cmp -s "$TMP/img2" "$TMP/img3"; then
	echo "image idempotence OK ($(stat -c%s "$TMP/img2") bytes)"
else
	echo "IMAGE IDEMPOTENCE FAILED: img2 != img3"
	cmp "$TMP/img2" "$TMP/img3" | head -3
	exit 1
fi
# and the resaved image must still work
"$BUILD/st" -s "$TMP/img3" -e '3 + 4' </dev/null | grep -q 7 && echo "resaved image evaluates OK"
