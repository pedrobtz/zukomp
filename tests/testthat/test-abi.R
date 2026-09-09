# Symbol audit. The point of these tests is to fail loudly if a future miniz
# update re-adds the archive or PNG code that tools/vendor/manifest.tsv's
# define set is supposed to remove. See design 24, criterion 14.
# exported_symbols() lives in helper-abi.R.

test_that("no ZIP archive symbol survives the trim", {
  expect_length(grep("mz_zip", exported_symbols(), value = TRUE), 0L)
})

test_that("no PNG writer symbol survives the trim", {
  # Upstream guards these by MINIZ_NO_DEFLATE_APIS, which zukomp needs, so
  # they are removed by tools/patches/miniz/0001-guard-png-writer.patch.
  expect_length(grep("tdefl_write_image", exported_symbols(), value = TRUE), 0L)
})

test_that("no zlib-ABI name is exported", {
  # MINIZ_NO_ZLIB_COMPATIBLE_NAMES must stay set: miniz would otherwise define
  # compress/inflate/crc32/adler32 as file-scope statics in every translation
  # unit, colliding with the zlib R itself links.
  # Defined symbols only: an undefined reference (nm's "U") is something
  # this object *needs*, not something it exports, and only what is exported
  # can collide with the zlib the R process already links.
  syms <- grep("^\\s*U ", exported_symbols(), value = TRUE, invert = TRUE)
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
