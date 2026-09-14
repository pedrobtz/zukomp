# zu_decoder_reset(): public ABI that had no caller outside its own
# definition.
#
# It has more state to get right than the encoder's -- total_in/total_out and
# so every limit budget, the wrapper state machine, the gzip header parser,
# and miniz's own stream. The shape that matters is a keep-alive connection
# decoding a second response body through the handle that decoded the first.

test_that("a reset decoder decodes a second message identically to a fresh one", {
  for (codec in c("identity", "deflate-raw", "zlib", "gzip")) {
    for (kind in c("ascii", "utf8", "structured", "lcg")) {
      a <- new_payload(kind, 2000)
      b <- new_payload(kind, 3500)
      got <- zu_test_decoder_reset(komp_compress(a, codec),
                                   komp_compress(b, codec), codec)
      expect_identical(got, c(a, b), info = paste(codec, kind))
    }
  }
})

test_that("the second message need not resemble the first", {
  # Everything that could carry over is different between the two: length,
  # content, compressibility, and for gzip the whole header/trailer cycle.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    a <- new_payload("ascii", 30000)     # highly compressible
    b <- new_payload("lcg", 17)          # incompressible, and tiny
    got <- zu_test_decoder_reset(komp_compress(a, codec),
                                 komp_compress(b, codec), codec)
    expect_identical(got, c(a, b), info = codec)
  }
})

test_that("an empty message on either side of the reset still works", {
  for (codec in c("identity", "deflate-raw", "zlib", "gzip")) {
    x <- new_payload("ascii", 1000)
    expect_identical(
      zu_test_decoder_reset(komp_compress(raw(0), codec),
                            komp_compress(x, codec), codec),
      x, info = codec)
    expect_identical(
      zu_test_decoder_reset(komp_compress(x, codec),
                            komp_compress(raw(0), codec), codec),
      x, info = codec)
  }
})

test_that("the limit budget restarts rather than carrying over", {
  # This is the one that would bite zuhttp. total_out is per-stream, so a
  # cap that the first message nearly spends must be fully available to the
  # second. Carrying it over would make the second response on a connection
  # fail a limit the first had already used.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    a <- new_payload("ascii", 4000)
    b <- new_payload("ascii", 4000)
    # A cap that comfortably fits either message alone, but not both.
    cap <- 5000
    got <- zu_test_decoder_reset(komp_compress(a, codec),
                                 komp_compress(b, codec), codec,
                                 max_output = cap)
    expect_identical(got, c(a, b), info = codec)
  }
})

test_that("the reset limit is still enforced on the second message", {
  # The mirror of the test above: restarting the budget must not mean
  # abandoning it. A second message over the cap still fails.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    a <- new_payload("ascii", 100)
    b <- new_payload("ascii", 9000)
    expect_error(
      zu_test_decoder_reset(komp_compress(a, codec),
                            komp_compress(b, codec), codec,
                            max_output = 1000),
      class = "zukomp_output_limit", info = codec
    )
  }
})

test_that("resetting onto a different codec is refused", {
  # Reset re-parameterises one codec's stream; it does not switch codecs.
  # The vtable is fixed at zu_decoder_new() time, so swapping means a new
  # handle -- and a reset that appeared to succeed would decode the next
  # message with the wrong state machine.
  expect_error(zu_test_decoder_reset_codec("gzip", "zlib"),
               class = "zukomp_invalid_argument")
  expect_error(zu_test_decoder_reset_codec("zlib", "deflate-raw"),
               class = "zukomp_invalid_argument")
  expect_error(zu_test_decoder_reset_codec("identity", "gzip"),
               class = "zukomp_invalid_argument")

  # Naming the same codec is what a reset is, so that one succeeds.
  expect_true(zu_test_decoder_reset_codec("gzip", "gzip"))
})

test_that("a multi-member gzip stream survives a reset", {
  # The member state machine is the part most likely to be left mid-cycle:
  # ST_MEMBER_END decides whether another member follows, and a reset that
  # left that decision pending would mis-parse the next message's header.
  skip_if_not(komp_codec_available("gzip"))
  a <- new_payload("ascii", 800)
  b <- new_payload("utf8", 1200)
  multi <- c(komp_compress(a, "gzip"), komp_compress(b, "gzip"))
  single <- komp_compress(a, "gzip")

  expect_identical(zu_test_decoder_reset(multi, single, "gzip"),
                   c(a, b, a))
  expect_identical(zu_test_decoder_reset(single, multi, "gzip"),
                   c(a, a, b))
})

test_that("a reset after a failed message still decodes the next one", {
  # A decoder that hit an error must be usable again after a reset,
  # otherwise a single malformed response poisons the connection.
  skip_if_not(komp_codec_available("gzip"))
  x <- new_payload("ascii", 2000)
  good <- komp_compress(x, "gzip")

  bad <- good
  bad[[length(bad) - 4L]] <- as.raw(bitwXor(as.integer(bad[[length(bad) - 4L]]), 0xFFL))
  expect_error(zu_test_decoder_reset(bad, good, "gzip"),
               class = "zukomp_error")
})
