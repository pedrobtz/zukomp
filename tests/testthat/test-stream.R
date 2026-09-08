test_that("identity streams at every chunk boundary", {
  # 10007 is prime, so no chunk size divides it evenly and every sweep ends
  # on a partial chunk.
  x <- new_payload("ascii", 10007L)
  for (cin in chunk_sizes()) {
    for (cout in chunk_sizes()) {
      expect_chunked_roundtrip(x, "identity", cin, cout)
    }
  }
})

test_that("identity round-trips every payload kind", {
  withr::local_seed(20260907)
  for (kind in payload_kinds()) {
    x <- new_payload(kind, 4096L)
    expect_chunked_roundtrip(x, "identity", 31L, 32L)
  }
})

test_that("an empty stream round-trips", {
  # Zero-length input still has to reach ZU_FINISH and terminate cleanly
  # rather than spinning on "needs more input".
  x <- raw(0)
  got <- zu_test_stream(zu_test_stream(x, "identity", "encode"),
                        "identity", "decode")
  expect_identical(got, x)
  expect_length(got, 0L)
})

test_that("streaming and one-shot chunking agree exactly", {
  withr::local_seed(11L)
  x <- new_payload("structured", 8192L)
  whole <- zu_test_stream(x, "identity", "encode",
                          in_chunk = 1e6, out_chunk = 1e6)
  byte_at_a_time <- zu_test_stream(x, "identity", "encode",
                                   in_chunk = 1L, out_chunk = 1L)
  expect_identical(whole, byte_at_a_time)
})

test_that("an intermediate flush does not change the output", {
  # ZU_FLUSH must be a "emit what you have now" instruction, not a change of
  # encoding. For identity that is trivially true, and the test is here to
  # stay true for codecs where it is not.
  x <- new_payload("ascii", 5000L)
  plain <- zu_test_stream(x, "identity", "encode", in_chunk = 64L)
  flushed <- zu_test_stream(x, "identity", "encode", in_chunk = 64L,
                            flush_every = 3L)
  expect_identical(plain, flushed)
})

test_that("an unknown codec is rejected before any streaming happens", {
  expect_codec_error(
    zu_test_stream(raw(10), "nope", "decode"),
    "zukomp_unsupported_codec"
  )
})

test_that("a codec that is declared but absent cannot be streamed", {
  expect_codec_error(
    zu_test_stream(raw(10), "zstd", "decode"),
    "zukomp_unsupported_codec"
  )
})

test_that("resetting an encoder with a new level really changes the level", {
  # mz_deflateReset() re-runs tdefl_init() with the flags baked in at
  # mz_deflateInit2() time, so the level is not re-applied by a reset alone.
  # The symptom was output identical to the *old* level while the zlib
  # header's FLEVEL bits advertised the new one -- and zu_encoder_reset() is
  # exactly the call a keep-alive HTTP client makes between messages.
  withr::local_seed(20260908)
  x <- new_payload("ascii", 8192)

  for (codec in c("deflate-raw", "zlib", "gzip")) {
    second <- zu_test_encoder_reset(x, codec, level1 = 1L, level2 = 9L)
    expect_identical(second, komp_compress(x, codec, level = 9L),
                     info = paste("codec =", codec))
    expect_identical(komp_decompress(second, codec), x,
                     info = paste("codec =", codec))
  }
})

test_that("resetting without a level keeps the stream's own level", {
  withr::local_seed(20260908)
  x <- new_payload("ascii", 8192)

  second <- zu_test_encoder_reset(x, "zlib", level1 = 1L, level2 = NULL)
  expect_identical(second, komp_compress(x, "zlib", level = 1L))
})

test_that("a reset encoder is reusable at the same level", {
  withr::local_seed(20260908)
  x <- new_payload("lcg", 4096)

  second <- zu_test_encoder_reset(x, "gzip", level1 = 6L, level2 = 6L)
  expect_identical(second, komp_compress(x, "gzip", level = 6L))
})
