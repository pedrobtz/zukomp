#!/bin/sh
# Builds the libFuzzer targets.
#
#   fuzz/build.sh                     libFuzzer targets, into fuzz/build/
#   fuzz/build.sh --standalone        replay drivers, into fuzz/build-replay/
#   fuzz/build.sh && fuzz/run.sh 60   build, then 60s per target
#
# The default needs clang with libFuzzer, which Apple's clang does not ship.
# --standalone builds the same targets behind a main() that replays files,
# needing only ASan and UBSan -- enough to run the corpus as a regression
# suite anywhere, and to re-check a committed crasher.
#
# Nothing here involves R: the targets link the pure-C core directly, which
# is why src/zu_internal.h must stay free of R.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

CC=${CC:-clang}
MODE=${1:-fuzzer}
if [ "$MODE" = "--standalone" ]; then
    BUILD=fuzz/build-replay
else
    BUILD=fuzz/build
fi
mkdir -p "$BUILD"

# Same trim as src/Makevars. Fuzzing a differently-configured build would
# be fuzzing something users never run.
DEFINES="-DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ARCHIVE_WRITING_APIS \
-DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES \
-DMINIZ_NO_PNG_APIS"

# -fno-sanitize-recover so UBSan findings abort and the fuzzer records them,
# rather than printing and carrying on.
if [ "$MODE" = "--standalone" ]; then
    SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
    DRIVER="fuzz/standalone_main.c"
else
    SAN="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
    DRIVER=""
fi
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

printf 'built %d targets into %s\n' \
    "$(ls -1 "$BUILD" | wc -l | tr -d ' ')" "$BUILD"
