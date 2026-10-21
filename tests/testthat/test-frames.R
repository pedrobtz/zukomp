# Frame continuation for the framed codecs, zstd and lz4-frame
# (src/zu_frames.c). The same contract gzip's members have (design 17,
# test-members.R): a following frame is decoded under ZU_DEC_CONCAT_MEMBERS,
# skippable frames are stepped over, anything else after a frame is trailing
# data, and the classification never depends on where the buffers split.

test_that("concatenated frames decode as one stream", {
  for (codec in framed_codecs()) {
    a <- new_payload("ascii", 3000L)
    b <- new_payload("structured", 2000L)
    z <- c(komp_compress(a, codec), komp_compress(b, codec))
    expect_identical(komp_decompress(z, codec), c(a, b), info = codec)
    for (n in chunk_sizes()) {
      expect_identical(
        zu_test_stream(z, codec, "decode", in_chunk = n, out_chunk = n),
        c(a, b), info = paste(codec, "chunk", n))
    }
  }
})

test_that("skippable frames are stepped over, before, between and after", {
  for (codec in framed_codecs()) {
    a <- new_payload("ascii", 1000L)
    za <- komp_compress(a, codec)
    s <- skippable_frame(7L)
    for (z in list(c(s, za), c(za, s, za), c(za, s))) {
      want <- if (length(z) > length(za) + length(s)) c(a, a) else a
      expect_identical(komp_decompress(z, codec), want, info = codec)
      expect_identical(
        zu_test_stream(z, codec, "decode", in_chunk = 1L, out_chunk = 1L),
        want, info = paste(codec, "1-byte"))
    }
  }
})

test_that("bytes after a frame are trailing data at every chunking", {
  for (codec in framed_codecs()) {
    z <- komp_compress(new_payload("ascii", 500L), codec)
    # Including tails that begin like a frame magic and then diverge, which
    # is the case that has to be held across a buffer boundary.
    magic <- z[1:4]
    tails <- list(as.raw(0x00), as.raw(c(0x41, 0x42, 0x43, 0x44, 0x45)),
                  magic[1], magic[1:2], magic[1:3],
                  c(magic[1:3], as.raw(0x00)),
                  as.raw(c(0x50, 0x2a)))
    for (tail in tails) {
      for (n in c(1L, 2L, 3L, 4096L)) {
        expect_codec_error(
          zu_test_stream(c(z, tail), codec, "decode", in_chunk = n),
          "zukomp_trailing_bytes",
          info = sprintf("%s tail=%s chunk=%d", codec,
                         paste(tail, collapse = " "), n))
      }
      expect_codec_error(komp_decompress(c(z, tail), codec),
                         "zukomp_trailing_bytes", info = codec)
    }
  }
})

test_that("trailing data can be ignored, and the cursor stops at the frame", {
  for (codec in framed_codecs()) {
    x <- new_payload("ascii", 500L)
    z <- komp_compress(x, codec)
    tail <- as.raw(c(0x41, 0x42, 0x43))
    got <- zu_test_stream(c(z, tail), codec, "decode",
                          reject_trailing = FALSE, report_consumed = TRUE)
    expect_identical(as.vector(got), x, info = codec)
    expect_identical(attr(got, "consumed"), as.double(length(z)), info = codec)
  }
})

test_that("without concatenation the first frame is the stream", {
  for (codec in framed_codecs()) {
    a <- new_payload("ascii", 800L)
    za <- komp_compress(a, codec)
    z <- c(za, za)
    got <- zu_test_stream(z, codec, "decode", concat_members = FALSE,
                          reject_trailing = FALSE, report_consumed = TRUE)
    expect_identical(as.vector(got), a, info = codec)
    expect_identical(attr(got, "consumed"), as.double(length(za)), info = codec)
    expect_codec_error(
      zu_test_stream(z, codec, "decode", concat_members = FALSE),
      "zukomp_trailing_bytes")
  }
})

test_that("a stream ending exactly at max_output decodes at every chunking", {
  # The checksum still has to be read after the last output byte, with no
  # output room left, and libzstd may hold its last input byte back until
  # the output is flushed. Reporting that stall as output-starved turned a
  # valid stream into a limit error at some chunk sizes.
  x <- new_payload("structured", 5000L)
  for (codec in framed_codecs()) {
    z <- komp_compress(x, codec)
    for (n in c(chunk_sizes(), 100L)) {
      for (m in c(1L, 7L, 4096L)) {
        info <- sprintf("%s in=%d out=%d", codec, n, m)
        expect_identical(
          zu_test_stream(z, codec, "decode", in_chunk = n, out_chunk = m,
                         max_output = length(x)), x, info = info)
        expect_identical(
          zu_test_stream(z, codec, "decode", in_chunk = n, out_chunk = m,
                         expected_size = length(x)), x, info = info)
        expect_codec_error(
          zu_test_stream(z, codec, "decode", in_chunk = n, out_chunk = m,
                         max_output = length(x) - 1),
          "zukomp_output_limit", info = info)
      }
    }
  }
})

test_that("a frame cut after its last output byte is truncated, not a limit", {
  # The mirror image of the case above: all output delivered, no room left,
  # no input left, and the checksum missing. Answering "output-starved"
  # there reported truncation as an output limit into an exactly-sized sink,
  # which is how a consumer that knows the decoded length always calls.
  x <- new_payload("structured", 5000L)
  for (codec in framed_codecs()) {
    z <- komp_compress(x, codec)
    cut <- z[seq_len(length(z) - 2L)]
    expect_codec_error(zu_test_decompress_one(cut, codec, length(x)),
                       "zukomp_truncated", info = codec)
    expect_codec_error(
      zu_test_decompress_one(cut, codec, length(x), expected_size = length(x)),
      "zukomp_truncated", info = codec)
    for (n in c(1L, 7L, 4096L)) {
      expect_codec_error(
        zu_test_stream(cut, codec, "decode", in_chunk = n, out_chunk = n,
                       max_output = length(x)),
        "zukomp_truncated", info = paste(codec, "chunk", n))
    }
    # A sink one byte short is still the limit, not truncation.
    expect_codec_error(zu_test_decompress_one(z, codec, length(x) - 1L),
                       "zukomp_output_limit", info = codec)
  }
})

test_that("a second frame that is itself broken is an error, not a tail", {
  for (codec in framed_codecs()) {
    za <- komp_compress(new_payload("ascii", 800L), codec)
    cut <- za[seq_len(length(za) - 2L)]
    expect_codec_error(komp_decompress(c(za, cut), codec), "zukomp_truncated",
                       info = codec)
  }
})

test_that("detection recognises both magics and nothing headerless", {
  x <- new_payload("structured", 2000L)
  expect_identical(komp_detect(komp_compress(x, "zstd")), "zstd")
  expect_identical(komp_detect(komp_compress(x, "lz4-frame")), "lz4-frame")
  expect_identical(komp_detect(komp_compress(x, "lz4-block")), NA_character_)
  for (codec in framed_codecs()) {
    expect_identical(komp_decompress(komp_compress(x, codec)), x, info = codec)
  }
  # Three bytes of a magic are not enough.
  expect_identical(komp_detect(as.raw(c(0x28, 0xb5, 0x2f))), NA_character_)
  expect_identical(komp_detect(as.raw(c(0x04, 0x22, 0x4d))), NA_character_)
})

test_that("external zstd and lz4 frames are detected", {
  for (codec in framed_codecs()) {
    m <- fixture_manifest(codec)
    for (i in seq_len(nrow(m))) {
      expect_identical(komp_detect(fixture_bytes(codec, m$file[i])), codec,
                       info = m$file[i])
    }
  }
})

test_that("a reset decoder starts a fresh frame budget", {
  for (codec in framed_codecs()) {
    a <- new_payload("ascii", 3000L)
    b <- new_payload("structured", 3000L)
    got <- zu_test_decoder_reset(komp_compress(a, codec),
                                 komp_compress(b, codec), codec,
                                 max_output = 3000)
    expect_identical(got, c(a, b), info = codec)
  }
})

test_that("a reset encoder produces exactly what a fresh one does", {
  x <- new_payload("structured", 4000L)
  for (codec in c(framed_codecs(), "lz4-block")) {
    second <- zu_test_encoder_reset(x, codec, level1 = 1L, level2 = 9L)
    expect_identical(second, komp_compress(x, codec, level = 9L), info = codec)
    second <- zu_test_encoder_reset(x, codec, level1 = 9L)
    expect_identical(second, komp_compress(x, codec, level = 9L), info = codec)
  }
})
