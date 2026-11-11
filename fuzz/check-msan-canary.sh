#!/bin/sh
# Proves the MSan build detects before fuzz.yaml trusts its replay (#82).
#
#   fuzz/check-msan-canary.sh [path to msan_canary]
#
# The canary reproduces the shape of the bug miniz patch 0002 fixed: an
# unwritten malloc'd buffer, copied out, then read. It must fail, and fail
# for that reason. "It exited non-zero" is not enough: a canary that is
# missing, will not start, or dies on anything else exits non-zero too,
# and an MSan that never ran would then be reported as live. So the run
# must print MemorySanitizer's own report of an uninitialised read.
set -u
canary=${1:-fuzz/build-msan/msan_canary}
want='MemorySanitizer: use-of-uninitialized-value'

out=$("$canary" 2>&1)
status=$?
if [ "$status" -eq 0 ]; then
    echo "::error::msan_canary exited 0: MSan is not detecting uninitialised reads, so the replay proves nothing"
    exit 1
fi
if ! printf '%s\n' "$out" | grep -q "$want"; then
    echo "::error::msan_canary failed (status $status) without the report '$want', so its failure says nothing about MSan:"
    printf '%s\n' "$out" | tail -20
    exit 1
fi
echo "ok: the canary was caught by MSan ($want)"
