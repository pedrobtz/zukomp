# ZU_FLUSH across the codecs where it is not a no-op.
#
# Design 8: "ZU_FLUSH exists because a boolean `finish` cannot express 'put
# the bytes on the wire now' -- needed the moment zuhttp compresses a
# streaming request body."
#
# The only flush test used to be against identity, for which flush is
# trivially nothing. The real path is MZ_SYNC_FLUSH through miniz, and the
# property worth asserting is *not* byte-identity with unflushed output: a
# sync flush ends the current block and inserts an empty stored block, so
# flushed output is legitimately different and usually larger. What must
# hold is that it still decodes to the same input.

test_that("flushed output decodes to the same input, for every codec", {
  d <- komp_codecs()
  for (codec in d$id[which(d$available & d$can_encode & d$can_flush)]) {
    for (kind in c("ascii", "utf8", "structured", "lcg")) {
      x <- new_payload(kind, 9000)
      z <- zu_test_stream(x, codec, "encode", in_chunk = 256, out_chunk = 256,
                          flush_every = 2)
      expect_identical(zu_test_stream(z, codec, "decode"), x,
                       info = paste(codec, kind))
      # and through the ordinary whole-buffer decoder, which applies the
      # trailing-bytes and member policies a raw driver run does not
      expect_identical(komp_decompress(z, codec), x,
                       info = paste(codec, kind))
    }
  }
})

test_that("flushing at every call still produces a decodable stream", {
  # The pathological case: a flush per process() call, at one byte of input
  # and one byte of output room. Every wrapper byte and every sync-flush
  # marker then has to survive being emitted one byte at a time.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    x <- new_payload("ascii", 300)
    z <- zu_test_stream(x, codec, "encode", in_chunk = 1, out_chunk = 1,
                        flush_every = 1)
    expect_identical(komp_decompress(z, codec), x, info = codec)
  }
})

test_that("flush interval does not change what the stream decodes to", {
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    x <- new_payload("structured", 12000)
    outs <- lapply(c(1, 2, 3, 7, 16, 64), function(every) {
      zu_test_stream(x, codec, "encode", in_chunk = 512, out_chunk = 512,
                     flush_every = every)
    })
    for (z in outs) {
      expect_identical(komp_decompress(z, codec), x, info = codec)
    }
  }
})

test_that("a flush is not a finish", {
  # The distinction ZU_FLUSH exists for: flushing mid-stream must not
  # terminate the stream, so the bytes after the last flush are still part
  # of it and the whole thing decodes to the whole input.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    x <- new_payload("ascii", 4000)
    z <- zu_test_stream(x, codec, "encode", in_chunk = 128, out_chunk = 4096,
                        flush_every = 2)
    expect_identical(komp_decompress(z, codec), x, info = codec)
    # A flushed stream is a complete one: no trailing bytes left over.
    expect_silent(komp_decompress(z, codec))
  }
})

test_that("flushing costs size but never correctness", {
  # Documents the trade rather than pinning an exact size: a sync flush ends
  # the block and inserts an empty stored block, so frequent flushes make
  # the output larger. Asserting byte-identity with unflushed output -- what
  # the identity-only test implied -- would be wrong here.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    x <- new_payload("ascii", 20000)
    plain <- zu_test_stream(x, codec, "encode", in_chunk = 512)
    flushed <- zu_test_stream(x, codec, "encode", in_chunk = 512,
                              flush_every = 1)
    expect_gte(length(flushed), length(plain))
    expect_identical(komp_decompress(flushed, codec), x, info = codec)
    expect_identical(komp_decompress(plain, codec), x, info = codec)
  }
})

test_that("identity flush remains a genuine no-op", {
  # Kept from the original test: for a codec that copies bytes, a flush
  # really does change nothing, and that is worth still asserting.
  x <- new_payload("ascii", 5000L)
  expect_identical(
    zu_test_stream(x, "identity", "encode", in_chunk = 64L),
    zu_test_stream(x, "identity", "encode", in_chunk = 64L, flush_every = 3L)
  )
})

test_that("a decoder accepts ZU_FLUSH mid-stream", {
  # zu_decoder_process() checks ZU_CAN_FLUSH too. Decoding with flushes set
  # must not change what comes out.
  for (codec in c("identity", "deflate-raw", "zlib", "gzip")) {
    x <- new_payload("ascii", 6000)
    z <- komp_compress(x, codec)
    expect_identical(
      zu_test_stream(z, codec, "decode", in_chunk = 128, out_chunk = 128,
                     flush_every = 2),
      x, info = codec)
  }
})
