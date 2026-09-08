#!/bin/sh
# Replays the corpus through the standalone drivers under ASan and UBSan.
#
#   fuzz/replay.sh
#
# This is not fuzzing -- it finds nothing new. It is the regression half:
# every committed crasher, and every seed, must still come back clean.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

BUILD=fuzz/build-replay
[ -d "$BUILD" ] || { echo "run fuzz/build.sh --standalone first" >&2; exit 1; }

inputs=$(find fuzz/corpus -type f | sort)
[ -n "$inputs" ] || { echo "no corpus inputs" >&2; exit 1; }

status=0
for bin in "$BUILD"/fuzz_*; do
    # -f as well as -x: macOS leaves .dSYM *directories* beside each binary
    [ -f "$bin" ] && [ -x "$bin" ] || continue
    name=$(basename "$bin")
    # shellcheck disable=SC2086
    if out=$("$bin" $inputs 2>&1); then
        printf '    ok   %-26s %s\n' "$name" "$(echo "$out" | tail -1)"
    else
        printf '    FAIL %s\n' "$name"
        echo "$out" | tail -25
        status=1
    fi
done
exit "$status"
