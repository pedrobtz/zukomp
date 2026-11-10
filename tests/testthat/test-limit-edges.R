# Every decode limit at its edges: limit - 1, limit and limit + 1, for every
# codec, whole-buffer and streamed, at chunkings down to one byte
# (.claude/shared/correctness.md, "Straddle every threshold"; #64, #65).
#
# The driver enforces max_output and the expected size by shrinking the
# codec's window (zu_decoder_process(), CLAUDE.md "Limits shrink the codec's
# window"), so a stream that ends exactly at the limit is decoded with no
# output room left while its end-of-block code or trailer is still unread.
# Whether the codec then asks for input or for output decides between the
# payload and a limit error, and that is decided per chunk boundary: a test
# at one chunking finds it only by luck.

test_that("a DEFLATE-family stream ending exactly at a limit decodes at every input chunking", {
  # #64. The input chunk boundary falls after the last output byte and
  # before the end-of-block code or the trailer; deflate_process() answered
  # ZU_NEED_OUTPUT with no input left, and the driver read that as the cap
  # stopping the stream. The issue's reproducer is zlib at in_chunk = 30.
  x <- new_payload("ascii", 5000L)
  n <- length(x)
  outs <- if (slow_tests_on()) c(1L, 7L, 65536L) else 65536L
  for (codec in deflate_codecs()) {
    z <- komp_compress(x, codec)
    for (k in seq_along(z)) {
      for (m in outs) {
        info <- sprintf("%s in_chunk=%d out_chunk=%d", codec, k, m)
        expect_identical(
          decode_outcome(zu_test_stream(z, codec, "decode", in_chunk = k,
                                        out_chunk = m, max_output = n)),
          x, info = paste(info, "max_output = n"))
        expect_identical(
          decode_outcome(zu_test_stream(z, codec, "decode", in_chunk = k,
                                        out_chunk = m, expected_size = n)),
          x, info = paste(info, "expected_size = n"))
      }
    }
  }
})

test_that("a wrapper trailer cut after the last output byte is truncation, not a limit", {
  # The mirror image of the test above, as test-frames.R pins it for the
  # framed codecs: all output delivered, no room left, no input left, and
  # the trailer short. A short wrapper field is ZU_ERR_TRUNCATED (design 7),
  # whatever the output window was when the input ran out.
  x <- new_payload("ascii", 5000L)
  n <- length(x)
  for (codec in c("zlib", "gzip")) {
    z <- komp_compress(x, codec)
    cut <- z[seq_len(length(z) - 2L)]
    for (k in seq_along(cut)) {
      info <- sprintf("%s in_chunk=%d", codec, k)
      expect_identical(
        decode_outcome(zu_test_stream(cut, codec, "decode", in_chunk = k,
                                      out_chunk = 65536L, max_output = n)),
        "zukomp_truncated", info = paste(info, "max_output = n"))
      expect_identical(
        decode_outcome(zu_test_stream(cut, codec, "decode", in_chunk = k,
                                      out_chunk = 65536L, expected_size = n)),
        "zukomp_truncated", info = paste(info, "expected_size = n"))
    }
  }
})
