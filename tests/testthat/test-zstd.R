# zstd (RFC 8878), built in since #55. The framed-codec behaviour shared with
# lz4-frame -- frame continuation, trailing bytes, held magic prefixes -- is
# in test-frames.R; this file is zstd's own.

test_that("zstd round-trips every payload kind", {
  withr::local_seed(17L)
  for (kind in payload_kinds()) {
    x <- new_payload(kind, 4096L)
    expect_roundtrip(x, "zstd")
    expect_identical(komp_decompress(komp_compress(x, "zstd"), "zstd"), x,
                     info = kind)
  }
})

test_that("zstd round-trips at the edges of its level range", {
  x <- new_payload("structured", 6000L)
  row <- komp_codecs()[komp_codecs()$id == "zstd", ]
  for (lvl in c(row$level_min, -5L, 0L, 1L, row$level_default, 19L,
                row$level_max)) {
    expect_identical(
      komp_decompress(komp_compress(x, "zstd", level = lvl), "zstd"), x,
      info = paste("level", lvl)
    )
  }
  for (name in c("fast", "default", "best")) {
    expect_identical(
      komp_decompress(komp_compress(x, "zstd", level = name), "zstd"), x,
      info = name
    )
  }
})

test_that("zstd's advertised levels are libzstd's", {
  # level_min is a literal in the vtable; this is the check that it is
  # still ZSTD_minCLevel() for the vendored release.
  row <- komp_codecs()[komp_codecs()$id == "zstd", ]
  expect_identical(row$level_min, -131072L)
  expect_identical(row$level_max, 22L)
  expect_identical(row$level_default, 3L)
  expect_identical(row$level_fast, 1L)
  expect_identical(row$level_best, 19L)
  expect_codec_error(komp_compress(raw(4), "zstd", level = 23L),
                     "zukomp_invalid_argument")
  expect_codec_error(komp_compress(raw(4), "zstd", level = -131073L),
                     "zukomp_invalid_argument")
})

test_that("zstd streams correctly at every chunk boundary", {
  x <- new_payload("structured", 3001L)
  for (n in chunk_sizes()) {
    expect_chunked_roundtrip(x, "zstd", n, n)
  }
  expect_chunked_roundtrip(x, "zstd", 1L, 4096L)
  expect_chunked_roundtrip(x, "zstd", 4096L, 1L)
})

test_that("zstd output is a standard frame with a checksum", {
  x <- new_payload("ascii", 1000L)
  z <- komp_compress(x, "zstd")
  expect_identical(z[1:4], as.raw(c(0x28, 0xb5, 0x2f, 0xfd)))
  # Frame header descriptor (RFC 8878 3.1.1.1.1): bit 2 is the content
  # checksum flag, and the whole input arrived at once so bits 6-7 record a
  # content size.
  fhd <- as.integer(z[5])
  expect_true(bitwAnd(fhd, 0x04L) != 0L)
  expect_true(bitwAnd(fhd, 0xC0L) != 0L || bitwAnd(fhd, 0x20L) != 0L)
})

test_that("zstd compression is deterministic", {
  x <- new_payload("structured", 10000L)
  expect_identical(komp_compress(x, "zstd"), komp_compress(x, "zstd"))
  expect_identical(komp_compress(x, "zstd", level = 19L),
                   komp_compress(x, "zstd", level = 19L))
})

test_that("decodes frames made by the zstd command-line tool", {
  m <- fixture_manifest("zstd")
  expect_gt(nrow(m), 5L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    bytes <- fixture_bytes("zstd", row$file)
    expect_identical(zu_test_stream(bytes, "zstd", "decode"),
                     fixture_plaintext(row), info = row$file)
    expect_identical(zu_test_stream(bytes, "zstd", "decode",
                                    in_chunk = 1L, out_chunk = 1L),
                     fixture_plaintext(row), info = paste(row$file, "1-byte"))
  }
})

test_that("a corrupted content checksum is a checksum error", {
  x <- new_payload("ascii", 2048L)
  z <- komp_compress(x, "zstd")
  for (off in 0:3) {
    bad <- z
    i <- length(z) - off
    bad[i] <- as.raw(bitwXor(as.integer(bad[i]), 0xff))
    expect_codec_error(komp_decompress(bad, "zstd"), "zukomp_checksum_error",
                       info = paste("offset", off))
  }
})

test_that("a zstd frame cut short is truncated, at every position", {
  # libzstd sees the frame structure, so unlike a DEFLATE body it can tell
  # "needs more" from "is wrong" (design 7).
  x <- new_payload("structured", 3000L)
  z <- komp_compress(x, "zstd")
  # Every position under ZUKOMP_SLOW_TESTS, a sample otherwise.
  withr::local_seed(41L)
  for (cut in c(0L, truncation_positions(length(z)))) {
    expect_codec_error(komp_decompress(z[seq_len(cut)], "zstd"),
                       "zukomp_truncated", info = paste("cut at", cut))
  }
})

test_that("garbage is invalid data, not a crash", {
  expect_codec_error(komp_decompress(as.raw(1:64), "zstd"),
                     "zukomp_invalid_data")
  # The right magic followed by nonsense.
  bad <- c(as.raw(c(0x28, 0xb5, 0x2f, 0xfd)), as.raw(rep(0xff, 32)))
  expect_error(komp_decompress(bad, "zstd"), class = "zukomp_error")
})

test_that("bit flips in a zstd frame never decode silently wrong", {
  withr::local_seed(23L)
  x <- new_payload("structured", 4000L)
  z <- komp_compress(x, "zstd")
  for (i in seq_len(80)) {
    bad <- z
    pos <- sample(seq_along(bad), 1L)
    bad[pos] <- as.raw(bitwXor(as.integer(bad[pos]), 2^sample(0:7, 1L)))
    res <- tryCatch(komp_decompress(bad, "zstd", max_output = 1e6),
                    zukomp_error = function(e) NULL)
    # The content checksum is what makes this hold for zstd: whatever
    # decodes must be the original.
    expect_true(is.null(res) || identical(res, x), info = paste("pos", pos))
  }
})

test_that("max_output binds on a zstd bomb", {
  # 1 MiB of zeros compresses to a few dozen bytes.
  z <- komp_compress(raw(2^20), "zstd")
  expect_lt(length(z), 200L)
  expect_codec_error(komp_decompress(z, "zstd", max_output = 1000),
                     "zukomp_output_limit")
  for (n in c(1L, 7L, 4096L)) {
    expect_codec_error(
      zu_test_stream(z, "zstd", "decode", in_chunk = n, out_chunk = n,
                     max_output = 1000),
      "zukomp_output_limit", info = paste("chunk", n))
  }
  # Exactly at the cap is not an error.
  expect_identical(komp_decompress(z, "zstd", max_output = 2^20), raw(2^20))
})

test_that("max_ratio binds on a zstd bomb", {
  z <- komp_compress(raw(2^20), "zstd")
  expect_codec_error(komp_decompress(z, "zstd", max_ratio = 10),
                     "zukomp_ratio_limit")
})

test_that("a frame claiming a huge window is refused under max_output", {
  # Frame header with Single_Segment_Flag clear and Window_Descriptor
  # 0xC0: exponent 24, a 2^34-byte window that libzstd would have to
  # allocate before the first byte of output. Under a max_output that is the
  # limit refusing the stream; with no limit, libzstd's own 128 MiB ceiling.
  hdr <- as.raw(c(0x28, 0xb5, 0x2f, 0xfd, 0x00, 0xC0))
  body <- as.raw(c(0x01, 0x00, 0x00))   # a last, empty raw block
  z <- c(hdr, body)
  expect_codec_error(komp_decompress(z, "zstd", max_output = 2^20),
                     "zukomp_output_limit")
  expect_codec_error(komp_decompress(z, "zstd", max_output = NULL),
                     "zukomp_unsupported_codec")
})

test_that("a lying frame content size is invalid data", {
  # Single-segment frame declaring 5 bytes of content (FHD 0x20, FCS 1 byte)
  # whose only block is a raw block of 3 bytes.
  z <- as.raw(c(0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x05,
                0x19, 0x00, 0x00, 0x61, 0x62, 0x63))
  expect_codec_error(komp_decompress(z, "zstd"), "zukomp_invalid_data")
  # ...and the same frame declaring 3 decodes.
  ok <- z
  ok[6] <- as.raw(0x03)
  expect_identical(komp_decompress(ok, "zstd"), charToRaw("abc"))
})

test_that("the empty payload is a valid zstd frame", {
  z <- komp_compress(raw(0), "zstd")
  expect_gt(length(z), 4L)
  expect_identical(komp_decompress(z, "zstd"), raw(0))
  expect_identical(zu_test_decompress_one(z, "zstd", 0), raw(0))
  # No bytes at all is not a frame.
  expect_codec_error(komp_decompress(raw(0), "zstd"), "zukomp_truncated")
})

test_that("zstd supports a mid-stream flush", {
  x <- new_payload("ascii", 9000L)
  z <- zu_test_stream(x, "zstd", "encode", in_chunk = 100L, flush_every = 3)
  expect_identical(komp_decompress(z, "zstd"), x)
})

test_that("the one-shot zstd paths agree with the whole-buffer ones", {
  x <- new_payload("structured", 5000L)
  z <- komp_compress(x, "zstd")
  expect_identical(zu_test_decompress_one(z, "zstd", length(x)), x)
  expect_codec_error(zu_test_decompress_one(z, "zstd", length(x) - 1L),
                     "zukomp_output_limit")
  # Incompressible input fits the advertised bound in one shot.
  withr::local_seed(3L)
  r <- new_payload("random", 70000L)
  expect_lte(length(komp_compress(r, "zstd")),
             zu_test_compress_bound(length(r), "zstd"))
})
