#!/bin/sh
# Proves that external decoders accept zukomp's output: gzip, zlib, zstd and
# lz4-frame through their own tools and Python, deflate-raw and lz4-block
# through Python (#69).
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
    # deflate-raw (#69): every level band -- 0 is stored blocks, 1 the fast
    # matcher, 6 and 9 the lazy one -- and streamed with a sync flush every
    # seventh call, which puts empty stored blocks mid-stream.
    for (lvl in c(0L, 1L, 6L, 9L)) {
      writeBin(zukomp:::zu_test_stream(x, "deflate-raw", "encode", level = lvl),
               file.path(dir, sprintf("%s.l%d.raw", kind, lvl)))
    }
    writeBin(zukomp:::zu_test_stream(x, "deflate-raw", "encode", in_chunk = 100,
                                     flush_every = 7),
             file.path(dir, sprintf("%s.s.raw", kind)))
    # lz4-block (#69): the acceleration, default and HC compressors, whole
    # and through the driver at small chunks, which is the staging and
    # gather path in src/codec_lz4.c rather than one liblz4 call. No flush:
    # a bare block cannot (komp_codecs()$can_flush is FALSE).
    for (lvl in c(-5L, 1L, 12L)) {
      writeBin(komp_compress(x, "lz4-block", level = lvl),
               file.path(dir, sprintf("%s.l%d.lz4b", kind, lvl)))
    }
    writeBin(zukomp:::zu_test_stream(x, "lz4-block", "encode", in_chunk = 100),
             file.path(dir, sprintf("%s.s.lz4b", kind)))
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

# deflate-raw and lz4-block (#69). Neither has a command-line decoder, so
# both are read in Python, which is required here for the same reason the
# zstd and lz4 tools are: CI installs it, and a skipped half is unchecked.
# PYTHON overrides the interpreter, for a local venv carrying lz4.
PYTHON=${PYTHON:-python3}
"$PYTHON" -c 'import lz4.block' 2>/dev/null || {
    echo "FAIL $PYTHON has no lz4 module (pip install lz4, or python3-lz4)" >&2
    exit 1
}
echo "==> python zlib (wbits=-15) and lz4.block accept zukomp deflate-raw and lz4-block output"
if "$PYTHON" - "$tmp" <<'PY'
import glob, os, re, sys, zlib
import lz4
import lz4.block

d = sys.argv[1]
bad = 0


def plain(f, ext):
    base = re.sub(r"\.[ls][-0-9]*\." + ext + "$", "", os.path.basename(f))
    return open(os.path.join(d, base + ".plain"), "rb").read()


def report(ok, what):
    global bad
    print("    %s %s" % ("ok  " if ok else "FAIL", what))
    bad += not ok


# RFC 1951 says nothing may follow the final block's end, so the reference
# is driven as a stream and must report eof with no unused bytes: a plain
# zlib.decompress() stops at BFINAL and would ignore anything zukomp wrote
# after it.
for f in sorted(glob.glob(os.path.join(d, "*.raw"))):
    want = plain(f, "raw")
    try:
        o = zlib.decompressobj(wbits=-15)
        got = o.decompress(open(f, "rb").read()) + o.flush()
        ok = got == want and o.eof and not o.unused_data
    except zlib.error:
        ok = False
    report(ok, os.path.basename(f))


# The LZ4 block format (lz4_Block_format.md, the "End of block conditions"),
# decoded from the specification rather than by liblz4. lz4.block is liblz4
# too, the library zukomp vendors (a different release); this decoder is the
# half that cannot share a mistake with it. It also checks the encoder's
# obligations, which liblz4's safe decoder does not: the block ends on a
# literals-only sequence, the last 5 bytes are literals, and the last match
# starts at least 12 bytes before the end.
class BlockError(Exception):
    pass


def lz4_block_spec_decode(src):
    out = bytearray()
    i, n = 0, len(src)
    last_match_start = None
    if n == 0:
        raise BlockError("empty block: the format needs at least one token")
    while True:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if i >= n:
                    raise BlockError("literal length runs past the block")
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        if i + lit > n:
            raise BlockError("literals run past the block")
        out += src[i:i + lit]
        i += lit
        if i == n:
            last_lits = lit
            break                      # the last sequence: literals only
        if i + 2 > n:
            raise BlockError("offset runs past the block")
        off = src[i] | (src[i + 1] << 8)
        i += 2
        if off == 0 or off > len(out):
            raise BlockError("offset %d outside the %d bytes decoded" %
                             (off, len(out)))
        ml = token & 15
        if ml == 15:
            while True:
                if i >= n:
                    raise BlockError("match length runs past the block")
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4                        # MINMATCH
        last_match_start = len(out)
        start = len(out) - off
        for k in range(ml):            # byte by byte: overlap is legal
            out.append(out[start + k])
    total = len(out)
    if last_lits < min(5, total):
        raise BlockError("last %d bytes are not all literals" % min(5, total))
    if last_match_start is not None and last_match_start > total - 12:
        raise BlockError("last match starts %d bytes before the end" %
                         (total - last_match_start))
    return bytes(out)


for f in sorted(glob.glob(os.path.join(d, "*.lz4b"))):
    want = plain(f, "lz4b")
    blob = open(f, "rb").read()
    try:
        ok = lz4.block.decompress(blob, uncompressed_size=len(want)) == want
    except Exception:
        ok = False
    try:
        ok = ok and lz4_block_spec_decode(blob) == want
    except BlockError as e:
        print("    spec: %s" % e)
        ok = False
    report(ok, os.path.basename(f))

print("    (lz4.block %s, liblz4 %s)" % (lz4.__version__,
                                          lz4.library_version_string()))
sys.exit(1 if bad else 0)
PY
then :; else status=1; fi

if [ "$status" -eq 0 ]; then
    echo "==> all external decoders accepted zukomp output"
fi
exit "$status"
