test_that("zlib detects Adler-32 corruption", {
  z <- zu_test_stream(new_payload("ascii", 4096L), "zlib", "encode")
  expect_codec_error(
    zu_test_stream(flip_byte(z, length(z)), "zlib", "decode"),
    "zukomp_checksum_error"
  )
})

test_that("every byte of the Adler-32 trailer is checked", {
  z <- zu_test_stream(new_payload("ascii", 4096L), "zlib", "encode")
  for (off in 0:3) {
    expect_codec_error(
      zu_test_stream(flip_byte(z, length(z) - off), "zlib", "decode"),
      "zukomp_checksum_error"
    )
  }
})

test_that("a corrupt zlib header is invalid data, not a checksum error", {
  # The header check must fire before any inflation happens, and must be
  # distinguishable from a payload that inflated to the wrong bytes.
  z <- zu_test_stream(new_payload("ascii", 1024L), "zlib", "encode")
  expect_codec_error(zu_test_stream(flip_byte(z, 1L), "zlib", "decode"),
                     "zukomp_invalid_data")
  expect_codec_error(zu_test_stream(flip_byte(z, 2L), "zlib", "decode"),
                     "zukomp_invalid_data")
})

test_that("a preset dictionary is refused rather than mis-decoded", {
  # FDICT set means a dictionary we do not have. The stream is well formed,
  # so this is unsupported rather than invalid.
  z <- zu_test_stream(new_payload("ascii", 512L), "zlib", "encode")
  hdr <- as.integer(z[1:2])
  flg <- bitwOr(hdr[2], 0x20)                     # set FDICT
  # keep the header a multiple of 31 so it fails on FDICT, not on FCHECK
  base <- bitwShiftL(hdr[1], 8) + bitwAnd(flg, 0xE0)
  flg <- bitwAnd(flg, 0xE0) + (31 - (base %% 31)) %% 31
  z[2] <- as.raw(flg)
  expect_codec_error(zu_test_stream(z, "zlib", "decode"),
                     "zukomp_unsupported_codec")
})

test_that("corruption in the compressed body never returns wrong bytes silently", {
  # Either it errors, or -- if the flip happened to stay decodable -- the
  # Adler-32 catches it. What must never happen is success with wrong data.
  # Classifying every outcome, rather than asserting only inside a branch,
  # keeps this test from quietly becoming an empty one.
  withr::local_seed(4242L)
  x <- new_payload("ascii", 4096L)
  z <- zu_test_stream(x, "zlib", "encode")
  body <- 3:(length(z) - 5L)

  outcomes <- vapply(sample(body, min(40L, length(body))), function(i) {
    got <- tryCatch(zu_test_stream(flip_byte(z, i), "zlib", "decode"),
                    zukomp_error = function(e) NULL)
    if (is.null(got)) "rejected" else if (identical(got, x)) "silently_wrong"
    else "decoded_differently"
  }, character(1))

  expect_false(any(outcomes == "silently_wrong"))
  expect_gt(sum(outcomes == "rejected"), 0L)
})

test_that("raw DEFLATE corruption is detected as invalid data", {
  # No checksum in RFC 1951, so this is the format's own structural
  # validation doing the work -- and a documented reason `auto` will not
  # guess raw DEFLATE.
  withr::local_seed(7L)
  x <- new_payload("structured", 8192L)
  z <- zu_test_stream(x, "deflate-raw", "encode")
  outcomes <- vapply(sample.int(length(z), min(40L, length(z))), function(i) {
    got <- tryCatch(zu_test_stream(flip_byte(z, i), "deflate-raw", "decode"),
                    zukomp_error = function(e) NULL)
    if (is.null(got)) "rejected" else if (identical(got, x)) "silently_wrong"
    else "decoded_differently"
  }, character(1))

  expect_false(any(outcomes == "silently_wrong"))
  expect_gt(sum(outcomes == "rejected"), 0L)
})
