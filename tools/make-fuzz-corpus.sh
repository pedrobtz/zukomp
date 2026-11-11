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

# Target-shaped seeds (#68). fuzz_stream_boundaries and fuzz_roundtrip read
# their first bytes as parameters, so a bare fixture reaches them only as
# noise: a gzip file's 1f 8b 08 asks stream_boundaries for zlib at a 32-byte
# split, and the decode stops at the first byte. These prefix each fixture
# with a header that names its own codec, a split from the list below, and
# each of the two ZU_FINISH positions (fuzz/fuzz_stream_boundaries.c and
# fuzz/fuzz_roundtrip.c document the layouts).
byte() { printf "\\$(printf '%03o' "$1")"; }
codec_index() {   # the targets' `% 7` order
    case "$1" in
        gzip) echo 0 ;; zlib) echo 1 ;; deflate-raw|malformed) echo 2 ;;
        zstd) echo 3 ;; lz4-frame) echo 4 ;; lz4-block) echo 5 ;; *) echo 6 ;;
    esac
}
splits="0 1 2 6 29 254"     # chunk - 1: 1, 2, 3, 7, 30 and 255 bytes
i=0
for f in tests/testthat/fixtures/*/*.bin; do
    [ -f "$f" ] || continue
    dir=$(basename "$(dirname "$f")")
    c=$(codec_index "$dir")
    name=$(echo "$f" | tr '/' '_')
    for order in 0 1; do
        set -- $splits
        shift $(( (i + order) % 6 )); a=$1
        set -- $splits
        shift $(( (i + 3 * order + 1) % 6 )); b=$1
        { byte "$a"; byte "$b"; byte $((c + 7 * order)); cat "$f"; } \
            > "$dest/split-$order-$name"
    done
    i=$((i + 1))
done
# Plaintexts for fuzz_roundtrip: every codec, both FINISH positions, a level
# and a split per seed. The fixtures' bytes are as good a plaintext as any,
# and they mix compressible headers with incompressible bodies.
i=0
for f in tests/testthat/fixtures/zstd/*.bin tests/testthat/fixtures/gzip/py_plain_ascii.bin; do
    [ -f "$f" ] || continue
    for c in 0 1 2 3 4 5 6; do
        order=$(( (i + c) % 2 ))
        { byte $((c + 8 * order)); byte $(( (i * 3 + c) % 10 )); \
          byte $(( (i + c) % 4 * 31 )); byte $(( (i * 7 + c) % 5 * 17 )); \
          byte $(( (i + c) % 8 )); cat "$f"; } > "$dest/roundtrip-$c-$i"
    done
    i=$((i + 1))
done

# One regress-* seed per oracle mechanism that the fixtures do not already
# reach (#68). Each is the input a deliberately broken zukomp failed on, so
# replaying it is what keeps that oracle biting. fuzz/README.md lists them
# with the break each one catches.
reg() { cat > "$dest/regress-$1"; }
# fuzz_sniff: CINFO 8 is not zlib (RFC 1950 2.2), FDICT is; a skippable
# frame names neither zstd nor LZ4.
printf '\210\034abcdefgh' | reg sniff-zlib-cinfo8
printf '\170\273\000\000\000\001abcd' | reg sniff-zlib-fdict
printf 'P*M\030\004\000\000\000abcd' | reg sniff-skippable-frame
# fuzz_gzip_header: a wrong FHCRC and a reserved FLG bit.
printf '\037\213\010\002\000\000\000\000\000\377\000\000' | reg gzip-fhcrc-wrong
printf '\037\213\010\040\000\000\000\000\000\377' | reg gzip-flg-reserved
# fuzz_decode_zlib: a valid stream with its Adler-32 changed.
z=tests/testthat/fixtures/zlib/py_l6_ascii.bin
if [ -f "$z" ]; then
    n=$(wc -c < "$z" | tr -d ' ')
    { head -c $((n - 1)) "$z"; printf '\377'; } | reg zlib-adler-wrong
fi
# fuzz_decode_lz4_block: a match starting 4 bytes into a 13-byte block,
# under the 12 the format requires (#93).
printf '\100abcd\004\000\120vwxyz' | reg 93-lz4-block-end-rules
# fuzz_decode_raw: a dynamic header that ends early; read on as zero bits it
# has no end-of-block code (#74). Inside the #74 exemption until that lands.
printf '\235\004\000\000\000test' | reg 74-missing-eob
# fuzz_stream_boundaries: a fixed block split one byte at a time, which
# ends exactly at max_output (#64). Inside the #64 exemption until it lands.
f=tests/testthat/fixtures/malformed/raw-fixed-literals-ok.bin
if [ -f "$f" ]; then
    { byte 0; byte 254; byte 2; cat "$f"; } | reg 64-exact-cap
fi
printf 'seeded %d inputs into %s\n' "$(ls -1 "$dest" | wc -l | tr -d ' ')" "$dest"
