#!/bin/sh
# Proves the LinkingTo archive works end to end: install zukomp, build a real
# ZIP archive with zukomp's own codecs, then compile a C consumer against the
# installed miniz.h, link it against inst/lib/libzukomp.a, and read the
# archive back through the ZIP reader that zukomp.so deliberately does not
# contain.
#
# The unit tests in tests/testthat/test-linking.R audit symbols. This is the
# other half: that the header, the archive and the compiler agree, which a
# symbol table cannot show. Run from the package root.
set -eu

# Relative paths, not mktemp: this runs on Windows too, where the compiler is
# a native mingw binary that does not understand a Git Bash /tmp path.
LIB=./.check-linking/lib
WORK=./.check-linking/work
rm -rf ./.check-linking
mkdir -p "$LIB" "$WORK"
trap 'rm -rf ./.check-linking' EXIT

echo "==> installing zukomp"
R CMD INSTALL --preclean --no-multiarch --library="$LIB" . >/dev/null

echo "==> building a ZIP with zukomp's own codecs"
# No external zip program: CRAN guarantees none, and Windows runners have no
# usable one. The DEFLATE stream is what ZIP method 8 stores, and the CRC-32
# comes out of a gzip trailer, which is where zukomp already computes one.
WORK="$WORK" R_LIBS="$LIB" Rscript --vanilla -e '
  library(zukomp)
  payload <- charToRaw("zukomp reads ZIP containers\n")
  name <- charToRaw("hello.txt")

  deflated <- komp_compress(payload, "deflate-raw")
  gz <- komp_compress(payload, "gzip")
  crc <- gz[seq(length(gz) - 7L, length(gz) - 4L)]  # trailer: CRC-32, LE

  u16 <- function(v) as.raw(c(v %% 256L, v %/% 256L %% 256L))
  u32 <- function(v) as.raw(c(v %% 256L, v %/% 256L %% 256L,
                              v %/% 65536L %% 256L, v %/% 16777216L %% 256L))

  local <- c(as.raw(c(0x50, 0x4b, 0x03, 0x04)), u16(20L), u16(0L), u16(8L),
             u16(0L), u16(0L), crc, u32(length(deflated)), u32(length(payload)),
             u16(length(name)), u16(0L), name)
  central <- c(as.raw(c(0x50, 0x4b, 0x01, 0x02)), u16(20L), u16(20L), u16(0L),
               u16(8L), u16(0L), u16(0L), crc, u32(length(deflated)),
               u32(length(payload)), u16(length(name)), u16(0L), u16(0L),
               u16(0L), u16(0L), u32(0L), u32(0L), name)
  cd_offset <- length(local) + length(deflated)
  eocd <- c(as.raw(c(0x50, 0x4b, 0x05, 0x06)), u16(0L), u16(0L), u16(1L),
            u16(1L), u32(length(central)), u32(cd_offset), u16(0L))

  writeBin(c(local, deflated, central, eocd), file.path(Sys.getenv("WORK"), "probe.zip"))
  writeBin(payload, file.path(Sys.getenv("WORK"), "expected.txt"))
' >/dev/null

cat > "$WORK/probe.c" <<'PROBE_C'
/* The call sequence a format reader needs: open by path (so stdio has to be
   compiled in), locate a member by name, stream it out through an extraction
   iterator. None of these exist in zukomp.so. */
#include <miniz.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  mz_zip_archive zip;
  mz_zip_reader_extract_iter_state *iter;
  mz_uint32 index;
  char buf[256];
  size_t n, total = 0;

  if (argc != 3) return 2;
  memset(&zip, 0, sizeof(zip));

  if (!mz_zip_reader_init_file(&zip, argv[1], 0)) {
    fprintf(stderr, "init_file failed: %s\n",
            mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    return 1;
  }
  if (!mz_zip_reader_locate_file_v2(&zip, "hello.txt", NULL,
                                    MZ_ZIP_FLAG_CASE_SENSITIVE, &index)) {
    fprintf(stderr, "locate_file_v2 failed\n");
    return 1;
  }
  if ((iter = mz_zip_reader_extract_iter_new(&zip, index, 0)) == NULL) {
    fprintf(stderr, "extract_iter_new failed\n");
    return 1;
  }
  while ((n = mz_zip_reader_extract_iter_read(iter, buf + total,
                                              sizeof(buf) - total)) > 0) {
    total += n;
  }
  mz_zip_reader_extract_iter_free(iter);
  mz_zip_reader_end(&zip);

  {
    FILE *f = fopen(argv[2], "rb");
    char want[256];
    size_t wanted;
    if (!f) return 1;
    wanted = fread(want, 1, sizeof(want), f);
    fclose(f);
    if (wanted != total || memcmp(want, buf, total) != 0) {
      fprintf(stderr, "read %lu bytes, expected %lu\n",
              (unsigned long)total, (unsigned long)wanted);
      return 1;
    }
  }
  printf("ok\n");
  return 0;
}
PROBE_C

echo "==> compiling and linking the consumer"
CC=$(R CMD config CC)
CFLAGS=$(R CMD config CFLAGS)
# MINIZ_NO_ZLIB_COMPATIBLE_NAMES is not optional for a consumer: without it
# miniz.h defines compress/crc32/adler32 over this translation unit.
$CC $CFLAGS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -I"$LIB/zukomp/include" \
  -o "$WORK/probe" "$WORK/probe.c" "$LIB/zukomp/lib/libzukomp.a"

echo "==> reading the archive back"
[ "$("$WORK/probe" "$WORK/probe.zip" "$WORK/expected.txt")" = "ok" ] || {
  echo "FAIL: the linked consumer could not read the archive" >&2
  exit 1
}
echo "==> archive consumer builds, links and reads a real ZIP"
