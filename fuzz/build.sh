#!/bin/sh
# Builds the libFuzzer targets.
#
#   fuzz/build.sh                     libFuzzer targets, into fuzz/build/
#   fuzz/build.sh --standalone        replay drivers, into fuzz/build-replay/
#   fuzz/build.sh --msan              replay drivers + MSan, into fuzz/build-msan/
#   fuzz/build.sh && fuzz/run.sh 60   build, then 60s per target
#
# The default needs clang with libFuzzer, which Apple's clang does not ship.
# --standalone builds the same targets behind a main() that replays files,
# needing only ASan and UBSan -- enough to run the corpus as a regression
# suite anywhere, and to re-check a committed crasher.
#
# --msan is the uninitialised-read detector, and it exists because neither
# ASan nor UBSan is one: that is why the match-distance bug fixed by
# tools/patches/miniz/0002-validate-match-distance.patch survived a whole v1
# cycle of fuzzing. It reuses the *standalone* driver rather than libFuzzer
# on purpose -- libFuzzer is C++, and MSan on a binary linking an
# uninstrumented libc++ reports false positives inside libFuzzer itself, so
# searching under MSan would need an instrumented libc++ built first. Linux
# and real clang only; Apple's clang rejects -fsanitize=memory outright.
#
# Two things --msan depends on, both easy to break silently. The targets must
# *read* the bytes they decode, or uninitialised output is never reported --
# see zu_fuzz_consume() in fuzz_common.h. And msan_canary is built alongside
# to prove the detector is live: it reproduces the bug's shape and is
# required to fail. fuzz.yaml asserts exactly that.
#
# Nothing here involves R: the targets link the pure-C core directly, which
# is why src/zu_internal.h must stay free of R.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

CC=${CC:-clang}
MODE=${1:-fuzzer}
case "$MODE" in
    --standalone) BUILD=fuzz/build-replay ;;
    --msan)       BUILD=fuzz/build-msan ;;
    fuzzer)       BUILD=fuzz/build ;;
    *)            echo "unknown mode: $MODE" >&2; exit 2 ;;
esac
mkdir -p "$BUILD"

# Same trim as src/Makevars. Fuzzing a differently-configured build would
# be fuzzing something users never run.
DEFINES="-DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ARCHIVE_WRITING_APIS \
-DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES \
-DMINIZ_NO_PNG_APIS"
# zstd's and lz4's, likewise, with each bundled xxHash in its own namespace
# as src/Makevars arranges (the core gets zstd's set, which is inert for it).
ZSTD_DEFINES="-DXXH_NAMESPACE=ZSTD_ -DZSTD_DISABLE_ASM=1 \
-DZSTD_LEGACY_SUPPORT=0 -DZSTDLIB_VISIBLE= -DZSTDERRORLIB_VISIBLE="
LZ4_DEFINES="-DXXH_NAMESPACE=LZ4_ -DLZ4LIB_VISIBILITY= -DLZ4FLIB_VISIBILITY= \
-DNDEBUG"

# -fno-sanitize-recover so UBSan findings abort and the fuzzer records them,
# rather than printing and carrying on. MSan cannot be combined with ASan,
# and wants frame pointers and origin tracking to say *which* allocation an
# uninitialised byte came from -- without origins a report names the read
# but not the malloc, which for a 32 KiB dictionary is the whole question.
case "$MODE" in
    --standalone)
        SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
        DRIVER="fuzz/standalone_main.c"
        ;;
    --msan)
        SAN="-fsanitize=memory -fsanitize-memory-track-origins=2 \
-fno-omit-frame-pointer"
        DRIVER="fuzz/standalone_main.c"
        ;;
    *)
        SAN="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
        DRIVER=""
        ;;
esac
# Objects compiled ahead of the link take the same instrumentation, minus
# libFuzzer's main(), which only the final link may bring in.
LIB_SAN=$(printf '%s' "$SAN" | sed 's/-fsanitize=fuzzer,/-fsanitize=fuzzer-no-link,/')
INC="-Iinst/include -Isrc -Isrc/vendor/miniz -Isrc/vendor/zstd \
-Isrc/vendor/lz4 -Ifuzz"

CORE="src/zu_status.c src/zu_registry.c src/zu_buf.c src/zu_stream.c \
src/codec_identity.c src/codec_deflate.c src/zu_gzip.c src/zu_frames.c \
src/codec_zstd.c src/codec_lz4.c src/vendor/miniz/miniz.c"

# zstd and lz4 are compiled once, as objects, rather than per target: there
# are thirty of them, and lz4's need a different XXH_NAMESPACE from zstd's,
# which one compiler invocation cannot give two sets of sources. Upstream's
# own dialect, not -std=c99: vendored sources may use whatever they need.
OBJ="$BUILD/obj"
mkdir -p "$OBJ"
LIBS=""
for src in src/vendor/zstd/*/*.c; do
    o="$OBJ/zstd_$(basename "$src" .c).o"
    $CC -g -O1 $LIB_SAN -Isrc/vendor/zstd $ZSTD_DEFINES -c "$src" -o "$o"
    LIBS="$LIBS $o"
done
for src in src/vendor/lz4/*.c; do
    o="$OBJ/lz4_$(basename "$src" .c).o"
    $CC -g -O1 $LIB_SAN -Isrc/vendor/lz4 $LZ4_DEFINES -c "$src" -o "$o"
    LIBS="$LIBS $o"
done

for target in fuzz/fuzz_*.c; do
    name=$(basename "$target" .c)
    printf '==> %s\n' "$name"
    $CC -std=c99 -g -O1 $SAN $INC $DEFINES $ZSTD_DEFINES \
        "$target" $DRIVER $CORE $LIBS -o "$BUILD/$name"
done

# The canary is not a fuzz target -- it takes no input and must fail -- so it
# is built only here and deliberately not named fuzz_*, which is what keeps
# run.sh and replay.sh from picking it up as one.
if [ "$MODE" = "--msan" ]; then
    printf '==> msan_canary\n'
    $CC -std=c99 -g -O1 $SAN fuzz/msan_canary.c -o "$BUILD/msan_canary"
fi

# Count binaries, not the .dSYM *directories* macOS leaves beside each one.
printf 'built %d targets into %s\n' \
    "$(find "$BUILD" -maxdepth 1 -type f -name 'fuzz_*' | wc -l | tr -d ' ')" \
    "$BUILD"
