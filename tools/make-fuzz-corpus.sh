#!/bin/sh
# Seeds the fuzz corpus from the committed interop fixtures.
#
# Seeding matters more than it looks: starting from valid streams lets the
# fuzzer spend its budget mutating structure it has already reached, rather
# than trying to discover a valid gzip header by chance.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

dest=fuzz/corpus/seed
rm -rf "$dest"
mkdir -p "$dest"
for f in tests/testthat/fixtures/*/*.bin; do
    [ -f "$f" ] || continue
    cp "$f" "$dest/$(echo "$f" | tr '/' '_')"
done
printf 'seeded %d inputs into %s\n' "$(ls -1 "$dest" | wc -l | tr -d ' ')" "$dest"
