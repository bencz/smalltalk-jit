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

"$BUILD/st" -s "$TMP/img1" -b packages/Core </dev/null >/dev/null
ST_RESAVE="$TMP/img2" "$BUILD/st" -s "$TMP/img1" </dev/null >/dev/null
ST_RESAVE="$TMP/img3" "$BUILD/st" -s "$TMP/img2" </dev/null >/dev/null

if cmp -s "$TMP/img2" "$TMP/img3"; then
	echo "image idempotence OK ($(stat -c%s "$TMP/img2") bytes)"
else
	echo "IMAGE IDEMPOTENCE FAILED: img2 != img3"
	cmp "$TMP/img2" "$TMP/img3" | head -3
	exit 1
fi
# The bootstrap output is itself expected to be a fixpoint, so this is checked
# too rather than only img2 against img3: a writer that normalized something on
# the way back out would still pass the pair above.
if cmp -s "$TMP/img1" "$TMP/img2"; then
	echo "the bootstrap image is already a fixpoint"
else
	echo "IMAGE IDEMPOTENCE FAILED: img1 != img2 (the reload normalized something)"
	cmp "$TMP/img1" "$TMP/img2" | head -3
	exit 1
fi

# and the resaved image must still work. printNl, because -e evaluates and does
# not print: `3 + 4` on its own writes nothing and the grep would pass or fail
# for reasons that have nothing to do with the image.
"$BUILD/st" -s "$TMP/img3" -e '(3 + 4) printNl' </dev/null | grep -qx 7 \
	&& echo "resaved image evaluates OK"
