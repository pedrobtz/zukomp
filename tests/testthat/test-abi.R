# Symbol audit. The point of these tests is to fail loudly if a future miniz
# update re-adds the archive or PNG code that tools/vendor/manifest.tsv's
# define set is supposed to remove (design 24, criterion 14), or if the
# shared object starts exporting anything besides its init function (#34).
#
# The trim audits read compiled_symbols(), not exported_symbols(): with
# $(C_VISIBILITY) nothing of miniz's is exported, so an audit of the export
# list would pass with the ZIP reader compiled straight in. Both helpers, and
# the positive control that keeps the first one honest, live in helper-abi.R.

test_that("the shared object exports nothing but R_init_zukomp", {
  # Everything a caller reaches goes through a registration table: .Call
  # entry points through R_registerRoutines, the C ABI through
  # R_RegisterCCallable. An exported miniz could bind to another package's
  # vendored miniz in the same process instead (#34). This is the property
  # $(C_VISIBILITY) in src/Makevars exists for, and nothing else checks it.
  #
  # Skipped under an instrumented build, whose runtime exports symbols of its
  # own. The trim audits below still run there.
  skip_if(is_instrumented_build(),
          "instrumented build: its runtime exports symbols of its own")
  names <- sub("^.*[[:space:]]", "", exported_symbols())
  names <- sub("^_", "", names)          # Mach-O's leading underscore
  expect_identical(sort(names), "R_init_zukomp")
})

test_that("no ZIP archive symbol survives the trim", {
  # Of the shared object, which is the whole point of the two-build split in
  # src/Makevars: the ZIP reader exists, but only inside libzukomp.a,
  # compiled separately and linked into a consumer's binary rather than this
  # one. If a future change widens the trim in place instead, this fails.
  expect_length(grep("mz_zip", compiled_symbols(), value = TRUE), 0L)
})

test_that("no PNG writer symbol survives the trim", {
  # Upstream guards these by MINIZ_NO_DEFLATE_APIS, which zukomp needs, so
  # they are removed by tools/patches/miniz/0001-guard-png-writer.patch.
  expect_length(grep("tdefl_write_image", compiled_symbols(), value = TRUE), 0L)
})

test_that("no zlib-ABI name is compiled in", {
  # MINIZ_NO_ZLIB_COMPATIBLE_NAMES must stay set: miniz would otherwise define
  # compress/inflate/crc32/adler32 as file-scope functions in every
  # translation unit, colliding with the zlib R itself links.
  #
  # Against everything compiled in, not only what is exported. Hidden
  # visibility would stop an exported collision, but a zlib-named function
  # in here at all means the flag has been lost, and the header macros
  # (ZLIB_VERSION, MAX_WBITS) that come with it leak whatever the linker does.
  syms <- compiled_symbols()
  banned <- c("compress", "compressBound", "uncompress",
              "deflate", "deflateInit", "inflate", "inflateInit",
              "crc32", "adler32")
  for (name in banned) {
    expect_length(grep(paste0("\\b_?", name, "\\b"), syms, value = TRUE), 0L)
  }
})

test_that("miniz is compiled in at the pinned version", {
  # Must match version_string in tools/vendor/manifest.tsv; tools/ is not
  # installed, so tools/vendor/verify cross-checks this literal instead.
  vendored <- komp_info()$vendored
  expect_identical(vendored$version[vendored$source == "miniz"], "11.3.2")
})

test_that("zstd and lz4 are compiled in at the pinned versions", {
  # Same contract as miniz's: tools/vendor/verify checks these literals
  # against the manifest and against the vendored headers.
  vendored <- komp_info()$vendored
  expect_identical(vendored$version[vendored$source == "zstd"], "1.5.7")
  expect_identical(vendored$version[vendored$source == "lz4"], "1.10.0")
})

test_that("each bundled xxHash carries its own namespace", {
  # zstd and lz4 each bundle an xxHash, from different releases, and both
  # link into this one shared object. src/Makevars renames zstd's to ZSTD_
  # and lz4's to LZ4_; without that the two copies' XXH32/XXH64 collide at
  # link time on some toolchains and silently bind to one another on others.
  syms <- compiled_symbols()
  expect_true(any(grepl("\\b_?ZSTD_XXH64\\b", syms)))
  expect_true(any(grepl("\\b_?LZ4_XXH32\\b", syms)))
  expect_length(grep("[[:space:]]_?XXH(32|64)$", syms, value = TRUE), 0L)
})

test_that("no stdio, abort, exit or system RNG is linked", {
  # CRAN's compiled-code check rejects these whether or not they can run,
  # and three vendored libraries are the likeliest source of one: zstd and
  # lz4 print debug traces to stderr and assert() when their debug levels
  # are raised, and lz4's xxhash.c asserts unconditionally unless NDEBUG is
  # set. Read from the undefined-symbol table, which is what the linker
  # would resolve against libc.
  skip_if(is_instrumented_build(),
          "instrumented build: its runtime references symbols of its own")
  syms <- undefined_symbols()
  syms <- sub("@.*$", "", sub("^.*[[:space:]]", "", syms))  # glibc versions
  syms <- sub("^_+", "", syms)                               # Mach-O
  banned <- c("abort", "exit", "_exit", "printf", "fprintf", "vprintf",
              "vfprintf", "puts", "fputs", "putchar", "fputc", "fwrite",
              "stderr", "stdout", "stderrp", "stdoutp", "rand", "srand",
              "random", "srandom", "assert_rtn", "assert_fail", "assert")
  expect_identical(intersect(syms, banned), character())
})

test_that("the public header leaks no vendored codec vocabulary", {
  # design 8: zukomp.h must mention neither R nor miniz -- nor, since #55,
  # zstd or lz4. A leak here would put a vendored type in every downstream
  # package's translation unit.
  header <- installed_header_code()
  for (pattern in c("miniz", "mz_", "tdefl", "tinfl", "MZ_")) {
    expect_length(grep(pattern, header, fixed = TRUE, value = TRUE), 0L)
  }
  # As identifiers, not substrings: ZU_CODEC_LZ4_FRAME is zukomp's own name
  # for the codec and must stay.
  vendored <- "(^|[^A-Za-z0-9_])(ZSTD_|ZSTDLIB|LZ4_|LZ4F|LZ4HC|XXH)"
  expect_length(grep(vendored, header, value = TRUE), 0L)
})

test_that("the public header leaks no R vocabulary", {
  header <- installed_header_code()
  for (pattern in c("R.h", "Rinternals.h", "SEXP", "Rf_")) {
    expect_length(grep(pattern, header, fixed = TRUE, value = TRUE), 0L)
  }
})

test_that("the public header carries its guard and C++ wrapper", {
  header <- installed_header()
  expect_length(grep("^#ifndef ZUKOMP_H$", header), 1L)
  expect_length(grep("^extern \"C\" \\{$", header), 1L)
  expect_length(grep("ZUKOMP_ABI_VERSION 1", header, fixed = TRUE), 1L)
})

test_that("no DEFLATE vocabulary appears in a public type name", {
  # design 8: opaque handles are zu_encoder/zu_decoder, never
  # inflater/deflater, which would be codec-specific in a neutral header.
  header <- installed_header_code()
  for (pattern in c("inflater", "deflater")) {
    expect_length(grep(pattern, header, fixed = TRUE, value = TRUE), 0L)
  }
})
