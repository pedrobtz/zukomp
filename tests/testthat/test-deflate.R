test_that("round-trips the payload corpus at every level extreme", {
  for (codec in c("deflate-raw", "zlib")) {
    info <- komp_codecs()[komp_codecs()$id == codec, ]
    for (kind in payload_kinds()) {
      withr::local_seed(1L)
      x <- new_payload(kind, 8192L)
      for (lvl in c(info$level_min, info$level_default, info$level_max)) {
        expect_roundtrip(x, codec, lvl)
      }
    }
  }
})

test_that("both DEFLATE codecs advertise a 0..9 level axis", {
  d <- komp_codecs()
  for (codec in c("deflate-raw", "zlib")) {
    row <- d[d$id == codec, ]
    expect_true(row$available)
    expect_identical(row$level_min, 0L)
    expect_identical(row$level_max, 9L)
    expect_identical(row$level_default, 6L)
    expect_true(row$can_encode)
    expect_true(row$can_decode)
  }
})

test_that("a level outside the advertised range is rejected", {
  # Validated once in the driver, so no codec has to remember to do it.
  x <- new_payload("ascii", 256L)
  expect_codec_error(zu_test_stream(x, "zlib", "encode", level = 10L),
                     "zukomp_invalid_argument")
  expect_codec_error(zu_test_stream(x, "zlib", "encode", level = -1L),
                     "zukomp_invalid_argument")
})

test_that("higher levels do not produce larger output on compressible data", {
  x <- new_payload("ascii", 16384L)
  n1 <- length(zu_test_stream(x, "zlib", "encode", level = 1L))
  n9 <- length(zu_test_stream(x, "zlib", "encode", level = 9L))
  expect_lte(n9, n1)
})

test_that("zlib output is exactly raw DEFLATE plus its six wrapper bytes", {
  # 2-byte header, 4-byte Adler-32. If this drifts, the wrapper has grown
  # something it should not have.
  x <- new_payload("ascii", 4096L)
  raw_n <- length(zu_test_stream(x, "deflate-raw", "encode", level = 6L))
  zlib_n <- length(zu_test_stream(x, "zlib", "encode", level = 6L))
  expect_identical(zlib_n, raw_n + 6L)
})

test_that("streams correctly at pathological boundaries", {
  x <- new_payload("structured", 65537L)
  for (cin in chunk_sizes()) {
    expect_chunked_roundtrip(x, "zlib", cin, 1L)
  }
  for (cout in chunk_sizes()) {
    expect_chunked_roundtrip(x, "zlib", 1L, cout)
  }
})

test_that("raw DEFLATE streams at pathological boundaries too", {
  x <- new_payload("ascii", 20011L)
  for (cin in chunk_sizes()) {
    expect_chunked_roundtrip(x, "deflate-raw", cin, 7L)
  }
})

test_that("incompressible input round-trips through stored blocks", {
  # lcg expands slightly, which is the stored-block path -- a different code
  # path in every DEFLATE implementation and a classic source of bugs.
  x <- new_payload("lcg", 8192L)
  for (codec in c("deflate-raw", "zlib")) {
    z <- expect_roundtrip(x, codec, 9L)
    expect_gt(length(z), length(x))
  }
})

test_that("empty input round-trips", {
  for (codec in c("deflate-raw", "zlib")) {
    expect_roundtrip(raw(0), codec)
  }
})

test_that("chunking does not change the compressed bytes", {
  x <- new_payload("ascii", 12345L)
  whole <- zu_test_stream(x, "zlib", "encode", in_chunk = 1e6, out_chunk = 1e6)
  drip <- zu_test_stream(x, "zlib", "encode", in_chunk = 1L, out_chunk = 1L)
  expect_identical(whole, drip)
})
