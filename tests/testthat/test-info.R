test_that("komp_info() reports what was actually built", {
  info <- komp_info()
  # A string, not a package_version: komp_info() is diagnostic output, and
  # a package_version needs an as.character() at every paste() site.
  expect_type(info$version, "character")
  expect_length(info$version, 1L)
  expect_identical(info$version, as.character(utils::packageVersion("zukomp")))
  # still orderable, for anyone who needs that
  expect_true(package_version(info$version) >= package_version("0.1.0"))
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

  # MZ_ASSERT expands to assert() at 26 sites reachable from malformed
  # input. R supplies -DNDEBUG so they are compiled out of ordinary and CRAN
  # builds -- but a -UNDEBUG build (devtools' debug install) would abort()
  # the R session instead of raising a condition. This define is what makes
  # the shipped behaviour the same in both, so its absence is a real
  # regression rather than a cosmetic one.
  expect_true("MINIZ_NO_ASSERT" %in% flags)

  # The trim set is six upstream defines plus two of ours.
  expect_length(flags, 7L)
})

test_that("malformed input cannot reach a live assert", {
  # The whole point of MINIZ_NO_ASSERT: every one of these used to be an
  # abort() away from taking the R session down in a -UNDEBUG build. They
  # must come back as conditions.
  withr::local_seed(42)
  x <- new_payload("structured", 4000)
  z <- komp_compress(x, "gzip")

  for (i in seq_len(60)) {
    bad <- z
    pos <- sample(seq_along(bad), 1L)
    bad[[pos]] <- as.raw(bitwXor(as.integer(bad[[pos]]), 0xFFL))
    res <- tryCatch(komp_decompress(bad, "gzip", max_output = 1e6),
                    zukomp_error = function(e) NULL)
    # Either it decoded to something, or it raised a zukomp condition.
    # What it must never do is abort the process.
    expect_true(is.null(res) || is.raw(res))
  }
})
