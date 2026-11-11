# The encoder and decoder handles' lifecycle, design 9 (#71, #83).
#
# zu_encoder and zu_decoder outlive the call that made them: a C consumer
# holds one across calls, and resets it per message on a keep-alive
# connection. zu_test_stream() stops at the first terminal status, so the
# out-of-order sequences -- process after the end, after an error, finish
# twice, reset after a failure -- are reachable only through
# zu_test_sequence(), which scripts them step by step.
#
# What every test here asserts is the contract design 9 states, at every
# step, for every codec:
#
#   - after a stream fails, every later process() returns the same status,
#     consumes nothing and delivers nothing, until a reset;
#   - a reset returns a failed or ended handle to a fresh stream;
#   - a call the handle refuses before the stream sees it (no buffer, a bad
#     cursor, ZU_FLUSH on a codec that cannot flush, ZU_RUN after ZU_FINISH,
#     input to an ended encoder) changes nothing, and the stream carries on;
#   - after the end, an empty call returns ZU_STREAM_END again; input is
#     ZU_ERR_TRAILING for a decoder that rejects trailing bytes, and that
#     is a failure like any other;
#   - free is safe in every state, twice, and on NULL.
#
# Before the contract, the DEFLATE codecs happened to repeat their error;
# zstd and lz4-frame returned a different one per call and went on to decode
# fresh input after a failure; and every codec answered ZU_STREAM_END after
# an expected-size, trailing-data or output-limit failure.

test_that("an output-limit failure is sticky until reset, for every codec", {
  for (codec in lifecycle_codecs()) {
    expect_decoder_fails_then_recovers(
      codec, komp_compress(lc_payload(), codec), "ZU_ERR_OUTPUT_LIMIT",
      max_output = 1000
    )
  }
})

test_that("an expected-size failure is sticky until reset, for every codec", {
  # #83: the decoder was marked finished before the size was checked, so a
  # stream that ended short of (or past) its expected size answered
  # ZU_ERR_INVALID_DATA once and ZU_STREAM_END on the next call.
  for (codec in lifecycle_codecs()) {
    expect_decoder_fails_then_recovers(
      codec, komp_compress(lc_payload(), codec), "ZU_ERR_INVALID_DATA",
      expected_size = length(lc_good())
    )
    # Short as well as long: a two-byte identity stream told to expect 3 is
    # the review's reproducer.
    short <- komp_compress(lc_good()[1:150], codec)
    expect_decoder_fails_then_recovers(
      codec, short, "ZU_ERR_INVALID_DATA",
      expected_size = length(lc_good()), info = paste(codec, "short")
    )
  }
})

test_that("a trailing-data failure is sticky until reset, for every codec", {
  junk <- as.raw(c(0x00, 0x01, 0x02))
  # identity has no end of its own -- every byte is payload -- so its
  # stream ends where its input does and the junk is in the second shape.
  for (codec in setdiff(lifecycle_codecs(), c("identity", "lz4-block"))) {
    # The junk in the same call as the end of the stream.
    expect_decoder_fails_then_recovers(
      codec, c(komp_compress(lc_payload(), codec), junk), "ZU_ERR_TRAILING"
    )
  }
  for (codec in lifecycle_codecs()) {
    # The junk in a later call, after ZU_STREAM_END: a failure too, so the
    # outcome does not depend on where a buffer boundary fell. lz4-block
    # takes only this shape: a block has no end marker, so junk in the same
    # buffer is part of the block.
    z <- komp_compress(lc_payload(), codec)
    res <- zu_test_sequence(
      c(z, junk), codec, "decode",
      list(step_finish(length(z)), step_finish(3), step_finish(),
           step_process(0), step_nullbuf())
    )
    expect_identical(res$status[[1]], "ZU_STREAM_END", info = codec)
    expect_identical(res$bytes, lc_payload(), info = codec)
    expect_sticky(res, 2L, "ZU_ERR_TRAILING", info = codec)

    # Without ZU_DEC_REJECT_TRAILING the junk is not the stream's business:
    # the end stands, and nothing is consumed.
    res <- zu_test_sequence(
      c(z, junk), codec, "decode", reject_trailing = FALSE,
      list(step_finish(length(z)), step_finish(3), step_finish())
    )
    expect_identical(res$status, rep("ZU_STREAM_END", 3), info = codec)
    expect_identical(res$consumed[2:3], c(0, 0), info = codec)
    expect_identical(res$bytes, lc_payload(), info = codec)
  }
})

test_that("a ratio failure is sticky until reset, for every codec", {
  # identity's ratio is 1 by construction, so no ratio limit can fire.
  for (codec in setdiff(lifecycle_codecs(), "identity")) {
    expect_decoder_fails_then_recovers(
      codec, komp_compress(lc_payload(), codec), "ZU_ERR_RATIO_LIMIT",
      max_ratio = 2
    )
  }
})

test_that("a checksum failure is sticky until reset", {
  # The byte each format's checksum occupies: zlib's Adler-32 is the last
  # four bytes, gzip's CRC-32 the four before ISIZE, and zstd's and LZ4's
  # content checksums close the frame.
  where <- c(zlib = 0L, gzip = 7L, zstd = 0L, `lz4-frame` = 0L)
  for (codec in intersect(names(where), lifecycle_codecs())) {
    z <- komp_compress(lc_payload(), codec)
    i <- length(z) - where[[codec]]
    z[[i]] <- as.raw(bitwXor(as.integer(z[[i]]), 0x01L))
    expect_decoder_fails_then_recovers(codec, z, "ZU_ERR_CHECKSUM")
  }
})

test_that("a stream cut short stays failed when the rest arrives", {
  # The issue's case. zstd and lz4-frame reported the cut as
  # ZU_ERR_TRUNCATED and then decoded the remainder, ending in
  # ZU_STREAM_END: an error followed by success, from one stream. identity
  # has no structure to cut, so a short identity stream is a short stream.
  for (codec in setdiff(lifecycle_codecs(), "identity")) {
    z <- komp_compress(lc_payload(), codec)
    half <- length(z) %/% 2L
    res <- expect_decoder_fails_then_recovers(codec, z[seq_len(half)], NULL)
    # And the very shape that used to succeed: the rest of the same stream.
    res <- zu_test_sequence(
      z, codec, "decode",
      list(step_finish(half), step_process(length(z)), step_finish())
    )
    expect_sticky(res, 2L, res$status[[1]], info = codec)
  }
})

test_that("an injected codec failure is sticky until reset, for every codec", {
  # The core, not the codec, owns the contract: a failure entering where a
  # codec's own status would is handled the same as a data error.
  for (codec in lifecycle_codecs()) {
    x <- lc_payload()
    z <- komp_compress(x, codec)
    res <- zu_test_sequence(
      c(z, z), codec, "decode",
      list(step_inject("ZU_ERR_MEMORY"), step_process(10), step_process(10),
           step_finish(), step_nullbuf(), step_reset(length(z)),
           step_finish(length(z)))
    )
    expect_sticky(res, 2L, "ZU_ERR_MEMORY", to = 5L, info = codec)
    expect_identical(res$status[6:7], c("ZU_OK", "ZU_STREAM_END"), info = codec)
    expect_identical(res$bytes, x, info = codec)
  }
})

test_that("a decoder finishes twice, and an ended stream stays ended", {
  for (codec in lifecycle_codecs()) {
    x <- lc_payload()
    z <- komp_compress(x, codec)
    res <- zu_test_sequence(
      z, codec, "decode",
      list(step_finish(length(z)), step_finish(), step_finish(),
           step_process(0))
    )
    expect_identical(res$status, rep("ZU_STREAM_END", 4), info = codec)
    expect_identical(res$consumed, c(length(z), 0, 0, 0), info = codec)
    expect_identical(res$bytes, x, info = codec)
  }
})

test_that("ZU_RUN after ZU_FINISH is refused, and the stream carries on", {
  # ZU_FINISH says no more input follows (design 8). Before the contract a
  # later ZU_RUN was the codec's business: miniz's encoder called it
  # invalid data, liblz4's ended the frame and dropped the new input, and
  # libzstd took it into the same frame.
  for (codec in lifecycle_codecs()) {
    x <- lc_payload()
    for (mode in c("encode", "decode")) {
      input <- if (mode == "encode") x else komp_compress(x, codec)
      res <- zu_test_sequence(
        input, codec, mode,
        list(step_finish(length(input), room = 8),
             step_process(0), step_process(0, flush = "flush"),
             # The remainder the first call left, presented again.
             step_finish(length(input)))
      )
      info <- paste(codec, mode)
      expect_identical(res$status[[1]], "ZU_NEED_OUTPUT", info = info)
      expect_identical(res$status[2:3],
                       rep("ZU_ERR_INVALID_ARGUMENT", 2), info = info)
      expect_identical(res$produced[2:3], c(0, 0), info = info)
      expect_identical(res$status[[4]], "ZU_STREAM_END", info = info)
      out <- if (mode == "encode") komp_decompress(res$bytes, codec) else
        res$bytes
      expect_identical(out, x, info = info)
    }
  }
})

test_that("an encoder finishes twice and refuses input after the end", {
  for (codec in lifecycle_codecs()) {
    x <- lc_payload()
    res <- zu_test_sequence(
      c(x, x), codec, "encode",
      list(step_finish(length(x)), step_finish(), step_finish(5),
           step_finish(), step_reset(0), step_finish(length(x)))
    )
    # Input after the end is the caller's mistake, refused without harm:
    # the encoded stream is complete and valid, so it is not a failure.
    expect_identical(
      res$status,
      c("ZU_STREAM_END", "ZU_STREAM_END", "ZU_ERR_INVALID_ARGUMENT",
        "ZU_STREAM_END", "ZU_OK", "ZU_STREAM_END"),
      info = codec
    )
    expect_identical(res$consumed[2:4], c(0, 0, 0), info = codec)
    n1 <- res$produced[[1]]
    expect_identical(komp_decompress(res$bytes[seq_len(n1)], codec), x,
                     info = codec)
    expect_identical(komp_decompress(drop_first(res$bytes, n1), codec), x,
                     info = codec)
  }
})

test_that("an encoder failure is sticky until reset, for every codec", {
  # No input makes a built-in encoder fail, so the failure is injected
  # where the codec's status would enter.
  for (codec in lifecycle_codecs()) {
    x <- lc_payload()
    res <- zu_test_sequence(
      x, codec, "encode",
      list(step_process(1000), step_inject("ZU_ERR_INTERNAL"),
           step_process(1000), step_process(0), step_finish(),
           step_process(5, flush = "flush"), step_nullbuf(),
           step_reset(0), step_finish(length(x)))
    )
    expect_sticky(res, 3L, "ZU_ERR_INTERNAL", to = 7L, info = codec)
    expect_identical(res$status[8:9], c("ZU_OK", "ZU_STREAM_END"),
                     info = codec)
    n_before <- sum(res$produced[1:7])
    expect_identical(komp_decompress(drop_first(res$bytes, n_before), codec), x,
                     info = codec)
  }
})

test_that("refused calls change nothing, for encoders and decoders", {
  for (codec in lifecycle_codecs()) {
    x <- lc_payload()
    for (mode in c("encode", "decode")) {
      input <- if (mode == "encode") x else komp_compress(x, codec)
      flush <- if (lifecycle_can_flush(codec)) list() else
        list(step_process(10, flush = "flush"))
      res <- zu_test_sequence(
        input, codec, mode,
        c(list(step_nullbuf(), step_nullbuf("finish")), flush,
          list(step_finish(length(input))))
      )
      info <- paste(codec, mode)
      k <- length(res$status)
      expect_identical(res$status[1:2], rep("ZU_ERR_INVALID_ARGUMENT", 2),
                       info = info)
      if (k == 4L) {
        expect_identical(res$status[[3]], "ZU_ERR_UNSUPPORTED", info = info)
        expect_identical(res$consumed[[3]], 0, info = info)
      }
      expect_identical(res$status[[k]], "ZU_STREAM_END", info = info)
      out <- if (mode == "encode") komp_decompress(res$bytes, codec) else
        res$bytes
      expect_identical(out, x, info = info)
    }
  }
})

test_that("a zero-length call moves nothing and the stream carries on", {
  for (codec in lifecycle_codecs()) {
    x <- lc_payload()
    for (mode in c("encode", "decode")) {
      input <- if (mode == "encode") x else komp_compress(x, codec)
      res <- zu_test_sequence(
        input, codec, mode,
        list(step_process(0), step_process(0, room = 0),
             step_process(10), step_process(0),
             step_finish(length(input)))
      )
      info <- paste(codec, mode)
      # With room to write into and nothing to read, the stream asks for
      # input; with no room either it may ask for either. Neither is an
      # error and neither consumes a byte. A decoder has nothing to say
      # without input; an encoder may write its header (the DEFLATE
      # wrappers do), which is output, not progress through the input.
      expect_identical(res$status[[1]], "ZU_NEED_INPUT", info = info)
      expect_true(res$status[[2]] %in% c("ZU_NEED_INPUT", "ZU_NEED_OUTPUT"),
                  info = info)
      expect_identical(res$consumed[c(1, 2)], c(0, 0), info = info)
      if (mode == "decode") {
        expect_identical(res$produced[c(1, 2)], c(0, 0), info = info)
      }
      expect_identical(res$status[[5]], "ZU_STREAM_END", info = info)
      out <- if (mode == "encode") komp_decompress(res$bytes, codec) else
        res$bytes
      expect_identical(out, x, info = info)
    }
  }
})

test_that("any number of calls that move nothing leave the stream intact", {
  # libzstd counts calls that make no progress and, from the 16th in a row,
  # returns noForwardProgress_inputEmpty (no input) or _destFull (no room).
  # zukomp read either as corrupt data, so a caller polling a valid zstd
  # stream had it failed (fuzz_lifecycle found it). Waiting is not an
  # error, however long it lasts (design 8): 40 calls of each kind here,
  # well past 16.
  for (codec in lifecycle_codecs()) {
    x <- lc_payload()
    for (mode in c("encode", "decode")) {
      input <- if (mode == "encode") x else komp_compress(x, codec)
      idle <- rep(list(step_process(0)), 40)
      full <- rep(list(step_process(10, room = 0)), 40)
      res <- zu_test_sequence(
        input, codec, mode,
        c(list(step_process(10)), idle, full,
          list(step_finish(length(input))))
      )
      info <- paste(codec, mode)
      waits <- res$status[seq.int(2, 81)]
      expect_true(all(waits %in% c("ZU_NEED_INPUT", "ZU_NEED_OUTPUT", "ZU_OK")),
                  info = paste(info, ":", paste(unique(waits), collapse = ", ")))
      expect_identical(res$status[[82]], "ZU_STREAM_END", info = info)
      out <- if (mode == "encode") komp_decompress(res$bytes, codec) else
        res$bytes
      expect_identical(out, x, info = info)
    }
  }
})

test_that("free is safe mid-stream, after an error, twice, and on NULL", {
  # What a freed handle leaves behind is a NULL in the harness: later calls
  # are calls on NULL, which the ABI refuses rather than dereferences.
  # Whether free released everything is the sanitizer and valgrind jobs'
  # question, and the fuzz_lifecycle target's allocation balance.
  for (codec in lifecycle_codecs()) {
    x <- lc_payload()
    for (mode in c("encode", "decode")) {
      input <- if (mode == "encode") x else komp_compress(x, codec)
      info <- paste(codec, mode)
      mid <- zu_test_sequence(
        input, codec, mode,
        list(step_process(length(input) %/% 2L), step_free(),
             step_process(10), step_reset(0), step_free())
      )
      expect_identical(mid$status[3:4], rep("ZU_ERR_INVALID_ARGUMENT", 2),
                       info = info)
      failed <- zu_test_sequence(
        input, codec, mode,
        list(step_inject("ZU_ERR_MEMORY"), step_process(10), step_free(),
             step_free())
      )
      expect_identical(failed$status[[2]], "ZU_ERR_MEMORY", info = info)
      ended <- zu_test_sequence(
        input, codec, mode,
        list(step_finish(length(input)), step_free())
      )
      expect_identical(ended$status[[1]], "ZU_STREAM_END", info = info)
      # A fresh handle freed with no call at all.
      expect_identical(zu_test_sequence(input, codec, mode,
                                        list(step_free()))$status, NA_character_,
                       info = info)
    }
  }
})

test_that("a truncated DEFLATE-family stream delivers the same bytes whichever call carries ZU_FINISH", {
  # miniz's MZ_FINISH stopped a short body as soon as its input ran out,
  # with output it had already decoded still held back: ZU_FINISH with the
  # final bytes delivered less before the error than ZU_FINISH on a later
  # empty call (#68's FINISH-order oracle, fuzz/corpus/regressions/).
  payload <- new_payload("ascii", 200000L)
  room <- 65536
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    z <- komp_compress(payload, codec)
    cut <- z[seq_len(length(z) %/% 2L)]
    with_last <- zu_test_sequence(
      cut, codec, "decode",
      c(list(step_finish(length(cut), room)), rep(list(step_finish(0, room)), 8))
    )
    later <- zu_test_sequence(
      cut, codec, "decode",
      c(list(step_process(length(cut), room)),
        rep(list(step_process(0, room)), 8),
        rep(list(step_finish(0, room)), 8))
    )
    expect_identical(tail(with_last$status, 1), "ZU_ERR_INVALID_DATA", info = codec)
    expect_identical(tail(later$status, 1), "ZU_ERR_INVALID_DATA", info = codec)
    expect_gt(length(later$bytes), room)
    expect_identical(with_last$bytes, later$bytes, info = codec)
    expect_identical(with_last$bytes, payload[seq_along(with_last$bytes)], info = codec)
  }
})

test_that("a decoder that answers ZU_NEED_INPUT has no output left to give", {
  # Design 8: ZU_NEED_INPUT means "progress possible with more input". A
  # decoder that has taken every byte it was offered can still hold decoded
  # output -- miniz up to its 32 KiB dictionary, libzstd and liblz4 their
  # internal buffers -- and every streaming decoder answered ZU_NEED_INPUT
  # there anyway when the caller's window had filled. A caller that did what
  # the status said, and waited for the network, stalled with that output
  # owed; only more input, or ZU_FINISH, released it.
  #
  # So: the input arrives in pieces of k bytes, each followed by a call that
  # offers nothing. Whenever a piece is answered ZU_NEED_INPUT, the empty
  # call after it must produce nothing. Which k exposes it depends on the
  # codec, so several are swept.
  room <- 4096
  payloads <- list(zeros = raw(2^17), ascii = new_payload("ascii", 2^17))
  for (kind in names(payloads)) {
    x <- payloads[[kind]]
    for (codec in c("deflate-raw", "zlib", "gzip", "zstd", "lz4-frame")) {
      z <- komp_compress(x, codec)
      for (k in c(1, 7, 64)) {
        steps <- rep(list(step_process(k, room), step_process(0, room)),
                     length(z) %/% k + length(x) %/% room + 8L)
        res <- zu_test_sequence(z, codec, "decode",
                                c(steps, list(step_finish(1e9, room))))
        st <- unlist(res$status)
        pieces <- seq(1L, length(st) - 2L, by = 2L)
        owed <- pieces[st[pieces] == "ZU_NEED_INPUT" &
                         res$produced[pieces + 1L] > 0]
        info <- paste(kind, codec, "k =", k)
        expect_identical(owed, integer(0), info = info)
        expect_identical(tail(st, 1), "ZU_STREAM_END", info = info)
        expect_identical(res$bytes, x, info = info)
      }
    }
  }
})
