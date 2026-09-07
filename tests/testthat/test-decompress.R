test_that("the default output cap is finite", {
  # An unbounded default would make every caller a decompression-bomb
  # target by omission.
  expect_true(is.finite(eval(formals(komp_decompress)$max_output)))
  expect_identical(eval(formals(komp_decompress)$max_output), 1024^3)
})

test_that("the default cap is overridable by option", {
  withr::local_options(zukomp.max_output = 1024)
  bomb <- komp_compress(raw(1e6), "gzip")
  expect_codec_error(komp_decompress(bomb, "gzip"), "zukomp_output_limit")
})

test_that("max_output = 0 means unlimited, deliberately", {
  x <- new_payload("zeros", 200000L)
  expect_length(komp_decompress(komp_compress(x, "gzip"), "gzip", max_output = 0),
                200000L)
})

test_that("a decompression bomb is stopped without allocating its output", {
  bomb <- komp_compress(raw(10e6), "gzip")
  expect_lt(length(bomb), 15000L)
  expect_codec_error(komp_decompress(bomb, "gzip", max_output = 4096),
                     "zukomp_output_limit")
})

test_that("max_ratio is opt-in and off by default", {
  expect_null(eval(formals(komp_decompress)$max_ratio))
  # zeros compress far past any safe-looking ratio, and that is legitimate
  z <- komp_compress(new_payload("zeros", 100000L), "gzip")
  expect_length(komp_decompress(z, "gzip"), 100000L)
  expect_codec_error(komp_decompress(z, "gzip", max_ratio = 10),
                     "zukomp_ratio_limit")
})

test_that("trailing junk is rejected by the whole-buffer API", {
  z <- c(komp_compress(new_payload("ascii", 512L), "zlib"), as.raw(1:3))
  expect_codec_error(komp_decompress(z, "zlib"), "zukomp_trailing_bytes")
})

test_that("concatenated gzip members are joined by the whole-buffer API", {
  a <- komp_compress(charToRaw("hello "), "gzip")
  b <- komp_compress(charToRaw("world"), "gzip")
  expect_identical(komp_decompress(c(a, b), "gzip"), charToRaw("hello world"))
})

test_that("malformed arguments are rejected", {
  z <- komp_compress(raw(10), "gzip")
  expect_codec_error(komp_decompress(z, "gzip", max_output = -1),
                     "zukomp_invalid_argument")
  expect_codec_error(komp_decompress(z, "gzip", max_ratio = -1),
                     "zukomp_invalid_argument")
  expect_codec_error(komp_decompress(z, "gzip", max_output = NA),
                     "zukomp_invalid_argument")
})
