# A registered codec that gets stuck fails the call; it cannot hang the
# session. zukomp's drive loop caught only one stuck shape -- no output, the
# whole input offered, ZU_NEED_INPUT -- and spun on the rest until the user
# pressed Ctrl-C. The two codecs here (src/codec_stall.c) get stuck the
# other ways: ZU_OK with nothing moved, and ZU_NEED_INPUT with input unread.
#
# A regression would spin, not fail, so every call runs under an elapsed
# time limit: R checks it at the loop's interrupt checks, which turns a hang
# into an error this test reports.

with_deadline <- function(expr, seconds = 10) {
  setTimeLimit(elapsed = seconds, transient = TRUE)
  on.exit(setTimeLimit(elapsed = Inf), add = TRUE)
  tryCatch(expr, error = function(e) e)
}

test_that("a stuck codec is an internal error from komp_compress() and komp_decompress()", {
  # 10 bytes fits one slice. Decoding 200 KiB spans several 64 KiB input
  # slices, so the codec is stuck with input both offered and still to
  # come; komp_compress() offers its whole input in one slice, so encoding
  # is always the last slice, which the narrower old check already caught
  # for stall-lookahead but not for stall-ok.
  for (codec in c("stall-ok", "stall-lookahead")) {
    for (n in c(10L, 200000L)) {
      x <- as.raw(seq_len(n) %% 251L)
      for (call in list(
        function() zukomp::komp_compress(x, codec),
        function() zukomp::komp_decompress(x, codec)
      )) {
        err <- with_deadline(call())
        expect_s3_class(err, "zukomp_internal_error")
        expect_true(inherits(err, "zukomp_internal_error"),
                    info = paste(codec, n, conditionMessage(err)))
        expect_false(grepl("time limit", conditionMessage(err)),
                     info = paste(codec, n))
      }
    }
  }
})

test_that("a stuck codec is ZU_ERR_INTERNAL from the one-shot entry points", {
  x <- as.raw(1:100)
  for (codec in c("stall-ok", "stall-lookahead")) {
    res <- with_deadline(zukomptest::compress_one_via_c(x, codec))
    expect_identical(res$status, "ZU_ERR_INTERNAL", info = codec)
    res <- with_deadline(zukomptest::decompress_one_via_c(x, codec, cap = 1000))
    expect_identical(res$status, "ZU_ERR_INTERNAL", info = codec)
  }
})

test_that("one call that moves nothing is allowed: a codec may change state", {
  # The drive loop calls a codec stuck after two calls in a row that move
  # nothing. stall-once idles on its first call only, so it must decode;
  # without this, the allowance could fall to zero calls unnoticed.
  for (n in c(10L, 200000L)) {
    x <- as.raw(seq_len(n) %% 251L)
    z <- with_deadline(zukomp::komp_compress(x, "stall-once"))
    expect_identical(z, xor(x, as.raw(0x5A)), info = n)
    expect_identical(with_deadline(zukomp::komp_decompress(z, "stall-once")),
                     x, info = n)
  }
})
