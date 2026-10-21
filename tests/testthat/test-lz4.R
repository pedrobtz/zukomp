# LZ4, as two codecs (#55): lz4-frame, the self-describing frame format, and
# lz4-block, one bare block. Frame continuation shared with zstd is in
# test-frames.R, and the caller-supplied size lz4-block depends on in
# test-expected-size.R.

# -- lz4-frame ----------------------------------------------------------------

test_that("lz4-frame round-trips every payload kind", {
  withr::local_seed(19L)
  for (kind in payload_kinds()) {
    x <- new_payload(kind, 4096L)
    expect_roundtrip(x, "lz4-frame")
    expect_identical(komp_decompress(komp_compress(x, "lz4-frame"),
                                     "lz4-frame"), x, info = kind)
  }
})

test_that("both lz4 codecs share liblz4's level scale", {
  # Negative levels are acceleration, 1 the fast compressor, 2..12 HC. The
  # mapping inverts below zero, which is why fast and best are declared.
  d <- komp_codecs()
  for (codec in c("lz4-frame", "lz4-block")) {
    row <- d[d$id == codec, ]
    expect_identical(row$level_min, -65536L, info = codec)
    expect_identical(row$level_max, 12L, info = codec)
    expect_identical(row$level_default, 1L, info = codec)
    expect_identical(row$level_fast, 1L, info = codec)
    expect_identical(row$level_best, 12L, info = codec)
  }
})

test_that("lz4-frame round-trips across its level range", {
  x <- new_payload("structured", 70000L)   # more than one 64 KiB block
  for (lvl in c(-65536L, -10L, -1L, 0L, 1L, 2L, 3L, 9L, 12L)) {
    expect_identical(
      komp_decompress(komp_compress(x, "lz4-frame", level = lvl), "lz4-frame"),
      x, info = paste("level", lvl))
  }
  for (name in c("fast", "default", "best")) {
    expect_identical(
      komp_decompress(komp_compress(x, "lz4-frame", level = name), "lz4-frame"),
      x, info = name)
  }
  # HC actually does better than the fast compressor here, so the level
  # reaches the compressor rather than stopping at validation.
  expect_lt(length(komp_compress(x, "lz4-frame", level = 12L)),
            length(komp_compress(x, "lz4-frame", level = 1L)))
  expect_codec_error(komp_compress(x, "lz4-frame", level = 13L),
                     "zukomp_invalid_argument")
})

test_that("lz4-frame streams correctly at every chunk boundary", {
  x <- new_payload("structured", 3001L)
  for (n in chunk_sizes()) {
    expect_chunked_roundtrip(x, "lz4-frame", n, n)
  }
  # Several blocks, through a one-byte output window: the encoder's staging
  # buffer drains a byte at a time.
  y <- new_payload("ascii", 140000L)
  expect_chunked_roundtrip(y, "lz4-frame", 4096L, 1L)
  expect_chunked_roundtrip(y, "lz4-frame", 1L, 4096L)
})

test_that("lz4-frame output is a standard frame with a checksum", {
  x <- new_payload("ascii", 1000L)
  z <- komp_compress(x, "lz4-frame")
  expect_identical(z[1:4], as.raw(c(0x04, 0x22, 0x4d, 0x18)))
  flg <- as.integer(z[5])
  expect_identical(bitwAnd(flg, 0xC0L), 0x40L)   # version 01
  expect_true(bitwAnd(flg, 0x04L) != 0L)          # content checksum
  expect_true(bitwAnd(flg, 0x08L) != 0L)          # content size: whole input
})

test_that("lz4-frame compression is deterministic", {
  x <- new_payload("structured", 10000L)
  expect_identical(komp_compress(x, "lz4-frame"), komp_compress(x, "lz4-frame"))
})

test_that("decodes frames made by the lz4 command-line tool", {
  m <- fixture_manifest("lz4-frame")
  expect_gt(nrow(m), 5L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    bytes <- fixture_bytes("lz4-frame", row$file)
    expect_identical(zu_test_stream(bytes, "lz4-frame", "decode"),
                     fixture_plaintext(row), info = row$file)
    expect_identical(zu_test_stream(bytes, "lz4-frame", "decode",
                                    in_chunk = 1L, out_chunk = 1L),
                     fixture_plaintext(row), info = paste(row$file, "1-byte"))
  }
})

test_that("a corrupted lz4 content checksum is a checksum error", {
  x <- new_payload("ascii", 2048L)
  z <- komp_compress(x, "lz4-frame")
  for (off in 0:3) {
    bad <- z
    i <- length(z) - off
    bad[i] <- as.raw(bitwXor(as.integer(bad[i]), 0xff))
    expect_codec_error(komp_decompress(bad, "lz4-frame"),
                       "zukomp_checksum_error", info = paste("offset", off))
  }
  # The header checksum too: the descriptor's last byte.
  bad <- z
  hc <- 7L + 8L   # magic, FLG, BD, 8-byte content size, then HC
  bad[hc] <- as.raw(bitwXor(as.integer(bad[hc]), 0xff))
  expect_codec_error(komp_decompress(bad, "lz4-frame"), "zukomp_checksum_error")
})

test_that("an lz4 frame cut short is truncated, at every position", {
  x <- new_payload("structured", 3000L)
  z <- komp_compress(x, "lz4-frame")
  # Every position under ZUKOMP_SLOW_TESTS, a sample otherwise.
  withr::local_seed(41L)
  for (cut in c(0L, truncation_positions(length(z)))) {
    expect_codec_error(komp_decompress(z[seq_len(cut)], "lz4-frame"),
                       "zukomp_truncated", info = paste("cut at", cut))
  }
})

test_that("bit flips in an lz4 frame never decode silently wrong", {
  withr::local_seed(29L)
  x <- new_payload("structured", 4000L)
  z <- komp_compress(x, "lz4-frame")
  for (i in seq_len(80)) {
    bad <- z
    pos <- sample(seq_along(bad), 1L)
    bad[pos] <- as.raw(bitwXor(as.integer(bad[pos]), 2^sample(0:7, 1L)))
    res <- tryCatch(komp_decompress(bad, "lz4-frame", max_output = 1e6),
                    zukomp_error = function(e) NULL)
    expect_true(is.null(res) || identical(res, x), info = paste("pos", pos))
  }
})

test_that("a lying lz4 content size is invalid data", {
  x <- new_payload("ascii", 1000L)
  z <- komp_compress(x, "lz4-frame")
  # Content size is the 8 bytes after FLG and BD; claim one byte more. The
  # header checksum then has to be made right again, or the descriptor is
  # rejected before the size is ever believed -- and it is one byte, so try
  # them all rather than reimplement XXH32 here.
  bad <- z
  bad[7] <- as.raw(as.integer(bad[7]) + 1L)
  classes <- vapply(0:255, function(hc) {
    bad[15] <- as.raw(hc)
    err <- tryCatch(komp_decompress(bad, "lz4-frame"), zukomp_error = identity)
    if (is.raw(err)) "decoded" else class(err)[1]
  }, character(1))
  # Exactly one header checksum is accepted, and with it the lie is caught.
  expect_identical(sum(classes != "zukomp_checksum_error"), 1L)
  expect_identical(classes[classes != "zukomp_checksum_error"],
                   "zukomp_invalid_data")
})

test_that("a legacy-format lz4 stream is not lz4-frame", {
  # lz4 -l writes the legacy format, magic 0x184C2102, which liblz4's frame
  # API does not decode. Detection does not claim it either.
  legacy <- as.raw(c(0x02, 0x21, 0x4c, 0x18, 0x05, 0x00, 0x00, 0x00,
                     0x50, 0x68, 0x65, 0x6c, 0x6c, 0x6f))
  expect_codec_error(komp_decompress(legacy, "lz4-frame"), "zukomp_invalid_data")
  expect_identical(komp_detect(legacy), NA_character_)
})

test_that("max_output and max_ratio bind on an lz4 bomb", {
  z <- komp_compress(raw(2^20), "lz4-frame")
  expect_lt(length(z), 5000L)
  expect_codec_error(komp_decompress(z, "lz4-frame", max_output = 1000),
                     "zukomp_output_limit")
  for (n in c(1L, 7L, 4096L)) {
    expect_codec_error(
      zu_test_stream(z, "lz4-frame", "decode", in_chunk = n, out_chunk = n,
                     max_output = 1000),
      "zukomp_output_limit", info = paste("chunk", n))
  }
  expect_codec_error(komp_decompress(z, "lz4-frame", max_ratio = 10),
                     "zukomp_ratio_limit")
  expect_identical(komp_decompress(z, "lz4-frame", max_output = 2^20), raw(2^20))
})

test_that("the empty payload is a valid lz4 frame", {
  z <- komp_compress(raw(0), "lz4-frame")
  expect_identical(komp_decompress(z, "lz4-frame"), raw(0))
  expect_identical(zu_test_decompress_one(z, "lz4-frame", 0), raw(0))
  expect_codec_error(komp_decompress(raw(0), "lz4-frame"), "zukomp_truncated")
})

test_that("lz4-frame supports a mid-stream flush", {
  x <- new_payload("ascii", 9000L)
  z <- zu_test_stream(x, "lz4-frame", "encode", in_chunk = 100L,
                      flush_every = 3)
  expect_identical(komp_decompress(z, "lz4-frame"), x)
})

test_that("lz4-frame fits its advertised bound in one shot", {
  withr::local_seed(4L)
  r <- new_payload("random", 200000L)
  bound <- zu_test_compress_bound(length(r), "lz4-frame")
  expect_identical(
    komp_decompress(zu_test_compress_one(r, "lz4-frame"), "lz4-frame"), r)
  expect_lte(length(komp_compress(r, "lz4-frame")), bound)
})

# -- lz4-block ----------------------------------------------------------------

test_that("lz4-block round-trips every payload kind", {
  withr::local_seed(31L)
  for (kind in payload_kinds()) {
    x <- new_payload(kind, 4096L)
    expect_roundtrip(x, "lz4-block")
    expect_identical(komp_decompress(komp_compress(x, "lz4-block"),
                                     "lz4-block"), x, info = kind)
  }
})

test_that("lz4-block round-trips across its level range", {
  x <- new_payload("structured", 70000L)
  for (lvl in c(-65536L, -3L, 0L, 1L, 2L, 9L, 12L)) {
    expect_identical(
      komp_decompress(komp_compress(x, "lz4-block", level = lvl), "lz4-block"),
      x, info = paste("level", lvl))
  }
})

test_that("lz4-block streams through the driver at every chunk boundary", {
  # A block cannot be produced or consumed incrementally, so the codec
  # gathers its input -- but it must still be correct through any windows.
  x <- new_payload("structured", 3001L)
  for (n in chunk_sizes()) {
    expect_chunked_roundtrip(x, "lz4-block", n, n)
  }
})

test_that("lz4-block is a bare block: no magic, never detected", {
  z <- komp_compress(new_payload("ascii", 4096L), "lz4-block")
  expect_identical(komp_detect(z), NA_character_)
  expect_codec_error(komp_decompress(z, "auto"), "zukomp_undetectable_codec")
  expect_false(komp_codecs()$can_flush[komp_codecs()$id == "lz4-block"])
})

test_that("an lz4 block decodes to exactly what liblz4 promises", {
  # Hand-built: token 0x50 (5 literals, no match), then the literals.
  blk <- as.raw(c(0x50, 0x68, 0x65, 0x6c, 0x6c, 0x6f))
  expect_identical(komp_decompress(blk, "lz4-block"), charToRaw("hello"))
  # The empty block is one zero token.
  expect_identical(komp_decompress(as.raw(0x00), "lz4-block"), raw(0))
  expect_identical(komp_compress(raw(0), "lz4-block"), as.raw(0x00))
})

test_that("a truncated lz4 block never decodes to the original", {
  # A bare block has no length and no checksum, so a cut that happens to
  # land after a literal run is itself a valid, shorter block: lz4-block can
  # only promise "an error, or different bytes", and says so. Told the
  # size, every cut is an error.
  x <- new_payload("structured", 3000L)
  z <- komp_compress(x, "lz4-block")
  # Every position under ZUKOMP_SLOW_TESTS, a sample otherwise.
  withr::local_seed(41L)
  for (cut in c(0L, truncation_positions(length(z)))) {
    res <- tryCatch(komp_decompress(z[seq_len(cut)], "lz4-block"),
                    zukomp_invalid_data = function(e) NULL)
    expect_false(identical(res, x), info = paste("cut at", cut))
    expect_codec_error(
      zu_test_stream(z[seq_len(cut)], "lz4-block", "decode",
                     expected_size = length(x)),
      "zukomp_invalid_data", info = paste("cut at", cut, "with size"))
  }
  # Bytes past the end of the block are not trailing data the codec can
  # stop before: a block has no end marker, so they make it malformed.
  expect_codec_error(komp_decompress(c(z, as.raw(0)), "lz4-block"),
                     "zukomp_invalid_data")
})

test_that("an lz4 block match reaching before the output is rejected", {
  # Token 0x11: one literal, then a five-byte match at offset 2 -- one byte
  # further back than anything decoded -- then the five closing literals.
  blk <- as.raw(c(0x11, 0x61, 0x02, 0x00, 0x50, 0x61, 0x61, 0x61, 0x61, 0x61))
  expect_codec_error(komp_decompress(blk, "lz4-block"), "zukomp_invalid_data")
})

test_that("an lz4 offset of 0 decodes to zeros, never to old memory", {
  # The block format calls offset 0 invalid, but liblz4 1.10.0 does not
  # reject it: it clears the match to zero bytes instead, which the format
  # document names as the reference behaviour. A recorded deviation, pinned
  # so that a change in either direction is noticed. What matters is the
  # half this asserts: the match is never a copy of whatever the output
  # buffer held -- for zukomp a malloc'd sink, so that would be heap
  # contents, the bug miniz's 0002 patch fixed for DEFLATE.
  # One literal, a five-byte match at offset 0, then twelve closing
  # literals (the format wants the last match to start 12 bytes from the
  # end, which liblz4 enforces once it knows where the end is).
  blk <- as.raw(c(0x11, 0x61, 0x00, 0x00, 0xC0, rep(0x62, 12)))
  want <- c(as.raw(0x61), raw(5), as.raw(rep(0x62, 12)))
  expect_identical(komp_decompress(blk, "lz4-block"), want)
  expect_identical(zu_test_stream(blk, "lz4-block", "decode",
                                  expected_size = 18), want)
})

test_that("bit flips in an lz4 block never escape as a crash", {
  withr::local_seed(37L)
  x <- new_payload("structured", 4000L)
  z <- komp_compress(x, "lz4-block")
  for (i in seq_len(80)) {
    bad <- z
    pos <- sample(seq_along(bad), 1L)
    bad[pos] <- as.raw(bitwXor(as.integer(bad[pos]), 2^sample(0:7, 1L)))
    # A bare block has no checksum, so a flipped literal can decode to
    # different bytes; what must never happen is anything but a raw vector
    # or a zukomp condition.
    res <- tryCatch(komp_decompress(bad, "lz4-block", max_output = 1e6),
                    zukomp_error = function(e) NULL)
    expect_true(is.null(res) || is.raw(res))
  }
})

test_that("max_output binds on an lz4-block bomb without the size", {
  z <- komp_compress(raw(2^20), "lz4-block")
  expect_lt(length(z), 5000L)
  # The decoder has to search for the size of a block nobody told it the
  # size of; max_output is what bounds the search.
  expect_codec_error(komp_decompress(z, "lz4-block", max_output = 1000),
                     "zukomp_output_limit")
  expect_codec_error(komp_decompress(z, "lz4-block", max_output = 2^20 - 1),
                     "zukomp_output_limit")
  expect_identical(komp_decompress(z, "lz4-block", max_output = 2^20), raw(2^20))
  expect_codec_error(komp_decompress(z, "lz4-block", max_ratio = 10),
                     "zukomp_ratio_limit")
  for (n in c(1L, 4096L)) {
    expect_codec_error(
      zu_test_stream(z, "lz4-block", "decode", in_chunk = n, out_chunk = n,
                     max_output = 1000),
      "zukomp_output_limit", info = paste("chunk", n))
  }
})

test_that("lz4-block fits its advertised bound in one shot", {
  withr::local_seed(5L)
  r <- new_payload("random", 100000L)
  z <- zu_test_compress_one(r, "lz4-block")
  expect_lte(length(z), zu_test_compress_bound(length(r), "lz4-block"))
  expect_identical(komp_decompress(z, "lz4-block"), r)
  # One byte short of the bound still works when the output is smaller...
  expect_identical(
    komp_decompress(zu_test_compress_one(new_payload("ascii", 5000L),
                                         "lz4-block", cap_delta = -1),
                    "lz4-block"),
    new_payload("ascii", 5000L))
})
