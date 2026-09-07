test_that("whole-buffer and streaming agree exactly", {
  # The roadmap's "no second code path" rule made concrete: if the
  # whole-buffer functions had their own loop, every chunk-boundary sweep in
  # this suite would be testing code no user ever runs.
  withr::local_seed(7L)
  x <- new_payload("random", 20000L)
  for (codec in c("gzip", "zlib", "deflate-raw", "identity")) {
    z <- komp_compress(x, codec)
    expect_identical(
      komp_decompress(z, codec),
      zu_test_stream(z, codec, "decode", in_chunk = 13L, out_chunk = 17L),
      info = codec
    )
    expect_identical(z, zu_test_stream(x, codec, "encode"), info = codec)
  }
})

test_that("compress round-trips every codec and payload kind", {
  withr::local_seed(99L)
  for (codec in komp_codecs()$id[komp_codecs()$available]) {
    for (kind in payload_kinds()) {
      x <- new_payload(kind, 4096L)
      expect_identical(komp_decompress(komp_compress(x, codec), codec), x,
                       info = paste(codec, kind))
    }
  }
})

test_that("gzip is the default codec", {
  x <- new_payload("ascii", 1024L)
  expect_identical(komp_compress(x), komp_compress(x, "gzip"))
  expect_identical(komp_detect_magic(komp_compress(x))[1:2], as.raw(c(0x1f, 0x8b)))
})

test_that("character input is rejected in v1", {
  # Bytes in, bytes out is the whole contract. Accepting text would mean
  # guessing an encoding on the user's behalf.
  expect_codec_error(komp_compress("text", "gzip"), "zukomp_error")
  expect_codec_error(komp_compress(1:10, "gzip"), "zukomp_invalid_argument")
  expect_codec_error(komp_compress(NULL, "gzip"), "zukomp_invalid_argument")
})

test_that("levels are validated against the codec's own range", {
  x <- new_payload("ascii", 256L)
  expect_codec_error(komp_compress(x, "gzip", level = 99), "zukomp_invalid_argument")
  expect_codec_error(komp_compress(x, "gzip", level = -1), "zukomp_invalid_argument")
  expect_codec_error(komp_compress(x, "gzip", level = 1.5), "zukomp_invalid_argument")
  # identity has no level axis at all
  expect_codec_error(komp_compress(x, "identity", level = 3), "zukomp_invalid_argument")
  expect_silent(komp_compress(x, "identity", level = NULL))
})

test_that("an unknown codec is rejected before any work happens", {
  expect_codec_error(komp_compress(raw(4), "nope"), "zukomp_unsupported_codec")
  expect_codec_error(komp_decompress(raw(4), "nope"), "zukomp_unsupported_codec")
})

test_that("empty input compresses and decompresses", {
  for (codec in c("gzip", "zlib", "deflate-raw", "identity")) {
    expect_identical(komp_decompress(komp_compress(raw(0), codec), codec), raw(0))
  }
})
