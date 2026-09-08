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
