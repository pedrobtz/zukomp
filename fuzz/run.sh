#!/bin/sh
# Runs each built target for N seconds (default 60) against its corpus.
#
#   fuzz/run.sh [seconds]
#
# A crash is written to fuzz/build/crash-*; minimise it, commit the
# minimised input to fuzz/corpus/regressions/, and add a testthat test that
# covers it. A fuzz finding without a regression test is a finding that can
# come back.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

SECS=${1:-60}
BUILD=fuzz/build
[ -d "$BUILD" ] || { echo "run fuzz/build.sh first" >&2; exit 1; }

status=0
for bin in "$BUILD"/fuzz_*; do
    [ -x "$bin" ] || continue
    name=$(basename "$bin")
    printf '==> %s (%ss)\n' "$name" "$SECS"
    if ! "$bin" fuzz/corpus/seed fuzz/corpus/regressions \
            -max_total_time="$SECS" -print_final_stats=1 \
            -rss_limit_mb=2560 -timeout=25 \
            -artifact_prefix="$BUILD/" > "$BUILD/$name.log" 2>&1; then
        printf '    FAILED -- see %s\n' "$BUILD/$name.log"
        tail -30 "$BUILD/$name.log"
        status=1
    else
        grep -E "^#[0-9]+.*DONE|stat::number_of_executed_units" \
            "$BUILD/$name.log" | tail -2 | sed 's/^/    /'
    fi
done
exit "$status"
