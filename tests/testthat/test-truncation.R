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

test_that("truncated names a cut wrapper field; a cut body is invalid_data", {
  # The rule, which was real but unwritten until design 7 recorded it:
  # zukomp reports zukomp_truncated exactly where its *own* wrapper code can
  # see that a fixed-size field did not arrive in full -- the RFC 1950 and
  # RFC 1952 header and trailer -- and zukomp_invalid_data everywhere inside
  # the DEFLATE body, because mz_inflate() collapses "needs more input" and
  # "corrupt" into MZ_DATA_ERROR.
  #
  # Pinned position by position rather than by counting classes: the counts
  # depend on how well the payload compresses, the rule does not. Until this
  # test the file asserted only the generic parent, so the rule could have
  # drifted in either direction without anything failing.
  x <- new_payload("ascii", 4096L)
  wrappers <- list(zlib = c(header = 2L, trailer = 4L),
                   gzip = c(header = 10L, trailer = 8L))
  for (codec in names(wrappers)) {
    w <- wrappers[[codec]]
    z <- komp_compress(x, codec)
    for (i in seq_len(length(z) - 1L)) {
      cls <- tryCatch({
        komp_decompress(z[seq_len(i)], codec)
        "accepted"
      }, zukomp_error = function(e) class(e)[1])
      # Cut before the header is complete, or after the body is complete but
      # before the trailer is: either way a wrapper field is short.
      in_wrapper <- i < w[["header"]] || i >= length(z) - w[["trailer"]]
      expect_identical(
        cls,
        if (in_wrapper) "zukomp_truncated" else "zukomp_invalid_data",
        info = sprintf("%s truncated to %d of %d bytes", codec, i, length(z))
      )
    }
  }
})

test_that("raw DEFLATE never reports truncation, having no wrapper", {
  # The other half of the same rule, and why deflate-raw tells a caller the
  # least about a failure: with no wrapper there is no fixed-size field whose
  # shortfall zukomp can recognise, so every position is invalid_data. A
  # caller needing "was this cut off?" has to get it from its transport.
  z <- komp_compress(new_payload("ascii", 4096L), "deflate-raw")
  for (i in seq_len(length(z) - 1L)) {
    expect_codec_error(komp_decompress(z[seq_len(i)], "deflate-raw"),
                       "zukomp_invalid_data",
                       info = sprintf("truncated to %d of %d", i, length(z)))
  }
})
