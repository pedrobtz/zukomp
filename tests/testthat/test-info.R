test_that("komp_info() reports what was actually built", {
  info <- komp_info()
  expect_identical(info$version, utils::packageVersion("zukomp"))
  expect_identical(info$abi_version, 1L)
  expect_true(all(c("identity", "deflate-raw", "zlib", "gzip") %in% info$codecs))
})

test_that("komp_info() records the vendored sources", {
  info <- komp_info()
  expect_s3_class(info$vendored, "data.frame")
  expect_true("miniz" %in% info$vendored$source)
  expect_match(info$vendored$version[info$vendored$source == "miniz"],
               "^[0-9]+\\.[0-9]+\\.[0-9]+$")
})

test_that("komp_info() records the miniz trim", {
  # The define set is a security property -- it is what removes the ZIP and
  # PNG code -- so it belongs in the build report, not just in a Makefile.
  flags <- komp_info()$build_flags
  expect_true("MINIZ_NO_ZLIB_COMPATIBLE_NAMES" %in% flags)
  expect_true("MINIZ_NO_ARCHIVE_APIS" %in% flags)
  expect_true("MINIZ_NO_PNG_APIS" %in% flags)
})
