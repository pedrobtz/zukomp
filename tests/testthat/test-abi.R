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

test_that("the public header leaks no vendored codec vocabulary", {
  # design 8: zukomp.h must mention neither R nor miniz. A leak here would
  # put a miniz type in every downstream package's translation unit.
  header <- installed_header_code()
  for (pattern in c("miniz", "mz_", "tdefl", "tinfl", "MZ_")) {
    expect_length(grep(pattern, header, fixed = TRUE, value = TRUE), 0L)
  }
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
