#!/bin/sh
# Proves that external decoders accept zukomp's output.
#
# The committed fixtures cover the other direction -- external encoders'
# bytes decoded by zukomp -- and run under R CMD check. This direction needs
# real external tools, which CRAN does not guarantee, so it is a CI job
# rather than a testthat test.
#
#   tools/check-interop.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "==> generating zukomp output"
Rscript -e '
  suppressMessages(devtools::load_all(quiet = TRUE))
  source("tests/testthat/helper-corpus.R")
  dir <- commandArgs(TRUE)[1]
  for (kind in c("ascii", "zeros", "utf8", "lcg", "structured", "empty")) {
    n <- if (kind == "empty") 0L else 8192L
    x <- new_payload(kind, n)
    writeBin(x, file.path(dir, paste0(kind, ".plain")))
    for (lvl in c(1L, 6L, 9L)) {
      writeBin(zukomp:::zu_test_stream(x, "gzip", "encode", level = lvl),
               file.path(dir, sprintf("%s.l%d.gz", kind, lvl)))
      writeBin(zukomp:::zu_test_stream(x, "zlib", "encode", level = lvl),
               file.path(dir, sprintf("%s.l%d.zz", kind, lvl)))
    }
    # zstd and lz4 frames, both as komp_compress() makes them (whole input,
    # content size recorded) and streamed in small chunks (no content size,
    # several blocks, a mid-stream flush), since the tools must accept both.
    for (lvl in c(-5L, 1L, 19L)) {
      writeBin(komp_compress(x, "zstd", level = lvl),
               file.path(dir, sprintf("%s.l%d.zst", kind, lvl)))
    }
    writeBin(zukomp:::zu_test_stream(x, "zstd", "encode", in_chunk = 100,
                                     flush_every = 7),
             file.path(dir, sprintf("%s.s.zst", kind)))
    for (lvl in c(-5L, 1L, 12L)) {
      writeBin(komp_compress(x, "lz4-frame", level = lvl),
               file.path(dir, sprintf("%s.l%d.lz4", kind, lvl)))
    }
    writeBin(zukomp:::zu_test_stream(x, "lz4-frame", "encode", in_chunk = 100,
                                     flush_every = 7),
             file.path(dir, sprintf("%s.s.lz4", kind)))
  }
' "$tmp"

status=0

echo "==> gzip -d accepts zukomp gzip output"
for f in "$tmp"/*.gz; do
    base=$(basename "$f" | sed 's/\.l[0-9]\.gz$//')
    if gzip -dc "$f" > "$tmp/out" 2>/dev/null && \
       cmp -s "$tmp/out" "$tmp/$base.plain"; then
        echo "    ok   $(basename "$f")"
    else
        echo "    FAIL $(basename "$f")"
        status=1
    fi
done

# The reference tools for the two framed codecs. Required, not optional: the
# CI job installs both, and a silently skipped half is how interop stops
# being checked.
for tool in zstd lz4; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "FAIL $tool(1) not found" >&2
        exit 1
    }
done

echo "==> zstd -d accepts zukomp zstd output"
for f in "$tmp"/*.zst; do
    base=$(basename "$f" | sed 's/\.[ls][-0-9]*\.zst$//')
    if zstd -q -d -c "$f" > "$tmp/out" 2>/dev/null && \
       cmp -s "$tmp/out" "$tmp/$base.plain"; then
        echo "    ok   $(basename "$f")"
    else
        echo "    FAIL $(basename "$f")"
        status=1
    fi
done

echo "==> lz4 -d accepts zukomp lz4-frame output"
for f in "$tmp"/*.lz4; do
    base=$(basename "$f" | sed 's/\.[ls][-0-9]*\.lz4$//')
    if lz4 -q -d -c "$f" > "$tmp/out" 2>/dev/null && \
       cmp -s "$tmp/out" "$tmp/$base.plain"; then
        echo "    ok   $(basename "$f")"
    else
        echo "    FAIL $(basename "$f")"
        status=1
    fi
done

if command -v python3 >/dev/null 2>&1; then
    echo "==> python zlib accepts zukomp zlib and gzip output"
    if python3 - "$tmp" <<'PY'
import glob, gzip, os, re, sys, zlib
d = sys.argv[1]
bad = 0
for f in sorted(glob.glob(os.path.join(d, "*.zz"))):
    base = re.sub(r"\.l\d\.zz$", "", os.path.basename(f))
    want = open(os.path.join(d, base + ".plain"), "rb").read()
    got = zlib.decompress(open(f, "rb").read())
    print("    %s %s" % ("ok  " if got == want else "FAIL", os.path.basename(f)))
    bad += got != want
for f in sorted(glob.glob(os.path.join(d, "*.gz"))):
    base = re.sub(r"\.l\d\.gz$", "", os.path.basename(f))
    want = open(os.path.join(d, base + ".plain"), "rb").read()
    got = gzip.decompress(open(f, "rb").read())
    bad += got != want
sys.exit(1 if bad else 0)
PY
    then :; else status=1; fi
else
    echo "note: python3 not found, skipping its half"
fi

if [ "$status" -eq 0 ]; then
    echo "==> all external decoders accepted zukomp output"
fi
exit "$status"
