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
  for (codec in c("deflate-raw", "zlib", "gzip", "zstd", "lz4-frame")) {
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
  for (codec in c("identity", "deflate-raw", "zlib", "gzip",
                  "zstd", "lz4-frame")) {
    x <- new_payload("ascii", 6000)
    z <- komp_compress(x, codec)
    expect_identical(
      zu_test_stream(z, codec, "decode", in_chunk = 128, out_chunk = 128,
                     flush_every = 2),
      x, info = codec)
  }
})

test_that("flush_every is a call interval, not a limit", {
  # It used to be validated by zu_check_limit(), which maps Inf to 0 -- and
  # 0 here means *never flush*, the exact opposite of the "Inf for no limit"
  # its message promises. A call interval has no unlimited spelling.
  x <- new_payload("ascii", 500)
  for (bad in list(Inf, 0, -1, 0.5, NA_real_, NaN)) {
    expect_error(
      zu_test_stream(x, "gzip", "encode", flush_every = bad),
      class = "zukomp_invalid_argument", info = format(bad)
    )
  }
  # A real interval still works.
  z <- zu_test_stream(x, "gzip", "encode", flush_every = 2)
  expect_identical(komp_decompress(z, "gzip"), x)
})

test_that("a flush's size is the data's, not the output window's (#91)", {
  # One ZU_FLUSH must finish when the bytes so far are on the wire. miniz
  # emits another empty stored block on every MZ_SYNC_FLUSH call with no new
  # input, so a codec that kept calling while miniz produced output filled
  # the whole window with them: 6000 bytes flushed every second call came
  # out as 118888 bytes at 4 KiB output chunks and 1900648 at 64 KiB, and a
  # C caller looping "while ZU_NEED_OUTPUT" never finished.
  x <- new_payload("ascii", 6000)
  d <- komp_codecs()
  for (codec in d$id[which(d$available & d$can_encode & d$can_flush)]) {
    small <- zu_test_stream(x, codec, "encode", in_chunk = 100,
                            out_chunk = 4096, flush_every = 2)
    large <- zu_test_stream(x, codec, "encode", in_chunk = 100,
                            out_chunk = 65536, flush_every = 2)
    plain <- zu_test_stream(x, codec, "encode", in_chunk = 100)
    expect_identical(small, large, info = codec)
    # 60 input slices, so 30 flushes: each costs a block boundary, an empty
    # block and the matches it cuts off -- 18 bytes for DEFLATE here --
    # never a window.
    expect_lt(length(small), length(plain) + 30 * 32, label = codec)
    expect_identical(komp_decompress(small, codec), x, info = codec)
  }
})

test_that("flushed output decodes before the stream finishes", {
  # A flush that leaves bytes inside the library, or is mapped to "carry
  # on", still round-trips once the stream is finished, which is all the
  # other flush tests check. What a streaming body needs is that the bytes
  # out after ZU_FLUSH decode, on their own, to everything fed so far --
  # with the decoder never told the stream has ended.
  x <- new_payload("ascii", 50000L)
  half <- 25000L
  tab <- komp_codecs()
  for (codec in tab$id[tab$available & tab$can_flush]) {
    enc <- zu_test_sequence(x, codec, "encode",
                            list(step_process(half, room = 1e6, flush = "flush")))
    dec <- zu_test_sequence(enc$bytes, codec, "decode",
                            list(step_process(length(enc$bytes), room = 1e6)))
    expect_identical(dec$bytes, x[seq_len(half)], info = codec)
  }
})
