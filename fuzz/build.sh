#!/bin/sh
# Builds the libFuzzer targets.
#
#   fuzz/build.sh                     libFuzzer targets, into fuzz/build/
#   fuzz/build.sh --standalone        replay drivers, into fuzz/build-replay/
#   fuzz/build.sh --msan              replay drivers + MSan, into fuzz/build-msan/
#   fuzz/build.sh --coverage          replay drivers + source-based coverage,
#                                     into fuzz/build-cov/ (fuzz/reach.sh)
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
    --coverage)   BUILD=fuzz/build-cov ;;
    fuzzer)       BUILD=fuzz/build ;;
    *)            echo "unknown mode: $MODE" >&2; exit 2 ;;
esac
mkdir -p "$BUILD"

# The defines are read from src/Makevars, not restated here (#81). A copy
# had drifted: it lacked MINIZ_NO_ASSERT, so the fuzzers ran a miniz whose
# MZ_ASSERT could abort where the shipping build raises a condition.
# Fuzzing a differently-configured build is fuzzing something users never
# run. tools/vendor/verify holds Makevars to the manifest's defines column,
# so this is the manifest's set too. -DNDEBUG is R's own (etc/Makeconf puts
# it in every package compile), so it is part of what ships.
#
# ZU_FUZZ_ASSERTS=1 is the extra diagnostic the review asked to keep: the
# same build with MINIZ_NO_ASSERT and NDEBUG dropped, so an assertion in a
# vendored library fires instead of being compiled out. It is never the
# only build that runs.
makevars_list() {
    # The -D/-U words of one Makevars variable, continuation lines included.
    awk -v var="$1" '
        $0 ~ "^" var "[ \t]*=" { on = 1; sub(/^[^=]*=/, "") }
        on { line = $0; cont = sub(/\\[ \t]*$/, "", line)
             n = split(line, w, /[ \t]+/)
             for (i = 1; i <= n; i++) if (w[i] ~ /^-[DU]/) printf "%s ", w[i]
             if (!cont) on = 0 }' src/Makevars
}
DEFINES="$(makevars_list PKG_CPPFLAGS) -DNDEBUG"
LZ4_DEFINES="$DEFINES $(makevars_list LZ4_CPPFLAGS) $(makevars_list LZ4_NDEBUG)"
if [ "${ZU_FUZZ_ASSERTS:-0}" = 1 ]; then
    strip_asserts() { printf '%s' "$1" | sed 's/-DMINIZ_NO_ASSERT//g; s/-DNDEBUG//g'; }
    DEFINES=$(strip_asserts "$DEFINES")
    LZ4_DEFINES=$(strip_asserts "$LZ4_DEFINES")
fi
case "$DEFINES" in
    *-DMINIZ_NO_ARCHIVE_APIS*) ;;
    *) echo "could not read the defines from src/Makevars" >&2; exit 1 ;;
esac
printf 'defines (src/Makevars): %s\n' "$DEFINES"
# System zlib is the DEFLATE family's reference decoder in fuzz_oracle.h.
# It is linked here and nowhere in the package (design 22 #11, #36).
ZLIB_LIBS=${ZLIB_LIBS:--lz}

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
    --coverage)
        # No sanitizer: the question is which lines the corpus runs, and an
        # oracle's abort would already have failed the replay job.
        SAN="-fprofile-instr-generate -fcoverage-mapping"
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
    $CC -g -O1 $LIB_SAN -Isrc/vendor/zstd $DEFINES -c "$src" -o "$o"
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
    $CC -std=c99 -g -O1 $SAN $INC $DEFINES \
        "$target" $DRIVER $CORE $LIBS $ZLIB_LIBS -o "$BUILD/$name"
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
