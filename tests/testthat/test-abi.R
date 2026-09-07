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
  syms <- exported_symbols()
  banned <- c("compressBound", "deflateInit", "inflateInit", "uncompress")
  for (name in banned) {
    expect_length(grep(paste0("\\b_?", name, "\\b"), syms, value = TRUE), 0L)
  }
})

test_that("miniz is compiled in at the pinned version", {
  # Must match version_string in tools/vendor/manifest.tsv; tools/ is not
  # installed, so tools/vendor/verify cross-checks this literal instead.
  expect_identical(zu_miniz_version(), "11.3.2")
})
