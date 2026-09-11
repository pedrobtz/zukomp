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
INC="-Iinst/include -Isrc -Isrc/vendor/miniz -Ifuzz"

CORE="src/zu_status.c src/zu_registry.c src/zu_buf.c src/zu_stream.c \
src/codec_identity.c src/codec_deflate.c src/zu_gzip.c \
src/vendor/miniz/miniz.c"

for target in fuzz/fuzz_*.c; do
    name=$(basename "$target" .c)
    printf '==> %s\n' "$name"
    $CC -std=c99 -g -O1 $SAN $INC $DEFINES \
        "$target" $DRIVER $CORE -o "$BUILD/$name"
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
