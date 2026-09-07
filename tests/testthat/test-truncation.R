# design 24 criterion 5: every truncation position of every representative
# stream must error, and none may report success. Truncation is the failure
# mode a network client actually hits, so it gets its own file.

test_that("truncation never reports success, for zlib", {
  withr::local_seed(20260907L)
  z <- zu_test_stream(new_payload("ascii", 4096L), "zlib", "encode")
  for (i in truncation_positions(length(z))) {
    expect_error(zu_test_stream(z[seq_len(i)], "zlib", "decode"),
                 class = "zukomp_error",
                 info = sprintf("truncated to %d of %d bytes", i, length(z)))
  }
})

test_that("truncation never reports success, for raw DEFLATE", {
  withr::local_seed(20260908L)
  z <- zu_test_stream(new_payload("ascii", 4096L), "deflate-raw", "encode")
  for (i in truncation_positions(length(z))) {
    expect_error(zu_test_stream(z[seq_len(i)], "deflate-raw", "decode"),
                 class = "zukomp_error",
                 info = sprintf("truncated to %d of %d bytes", i, length(z)))
  }
})

test_that("a zlib stream truncated inside its trailer is truncated, not corrupt", {
  # Losing trailer bytes is a length problem, not a checksum problem, and
  # zuhttp branches on the difference.
  z <- zu_test_stream(new_payload("ascii", 1024L), "zlib", "encode")
  for (drop in 1:4) {
    expect_codec_error(
      zu_test_stream(z[seq_len(length(z) - drop)], "zlib", "decode"),
      "zukomp_truncated"
    )
  }
})

test_that("a stream truncated to nothing errors", {
  expect_error(zu_test_stream(raw(0), "zlib", "decode"), class = "zukomp_error")
})

test_that("every truncation position is covered when slow tests are on", {
  skip_if_no_slow_tests()
  for (codec in c("zlib", "deflate-raw")) {
    z <- zu_test_stream(new_payload("structured", 2048L), codec, "encode")
    for (i in seq_len(length(z) - 1L)) {
      expect_error(zu_test_stream(z[seq_len(i)], codec, "decode"),
                   class = "zukomp_error", info = sprintf("%s @ %d", codec, i))
    }
  }
})
