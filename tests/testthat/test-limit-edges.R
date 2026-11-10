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
    expect_identical(
      sweep_mismatches(z, codec, x, seq_along(z), outs, max_output = n),
      character(0), info = paste(codec, "max_output = n"))
    expect_identical(
      sweep_mismatches(z, codec, x, seq_along(z), outs, expected_size = n),
      character(0), info = paste(codec, "expected_size = n"))
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
    expect_identical(
      sweep_mismatches(cut, codec, "zukomp_truncated", seq_along(cut), 65536L,
                       max_output = n),
      character(0), info = paste(codec, "max_output = n"))
    expect_identical(
      sweep_mismatches(cut, codec, "zukomp_truncated", seq_along(cut), 65536L,
                       expected_size = n),
      character(0), info = paste(codec, "expected_size = n"))
  }
})

test_that("every codec straddles max_output and expected_size, whole and streamed", {
  # #65. For each codec and both limits, the decoded size n - 1, n and n + 1
  # whole-buffer and at every chunking in limit_in_chunks(), against four
  # output windows: each outcome is the documented one, and the streamed
  # outcome is the whole-buffer one (a differential pair: two deliveries of
  # the same bytes the specification calls equivalent).
  x <- new_payload("ascii", 5000L)
  n <- length(x)
  for (codec in available_codecs()) {
    z <- komp_compress(x, codec)
    for (row in limit_straddle(n)) {
      want <- if (is.null(row$want)) x else row$want
      label <- sprintf("%s %s = n %+d", codec, row$arg, row$value - n)
      whole <- whole_outcome(z, codec, row, n)
      expect_identical(whole, want, info = paste(label, "whole"))
      # The sample at four output windows; in CI, every input chunking at
      # the widest one too. Refused at n - 1, no chunking may have handed
      # over more than n - 1 bytes first.
      args <- list(z, codec, whole, limit_in_chunks(z, all = FALSE),
                   c(1L, 7L, 4096L, 65536L))
      args[[row$arg]] <- row$value
      if (row$value < n) args$max_delivered <- row$value
      expect_identical(do.call(sweep_mismatches, args), character(0),
                       info = paste(label, "streamed"))
      if (slow_tests_on()) {
        args[c(4L, 5L)] <- list(seq_along(z), 65536L)
        expect_identical(do.call(sweep_mismatches, args), character(0),
                         info = paste(label, "streamed, every chunking"))
      }
    }
  }
})

test_that("max_ratio straddles the exact ratio, whole-buffer", {
  # A payload sized so the decoded length is an exact multiple of the
  # stream length, so the limit can sit on the ratio itself: r - 1 refuses,
  # r and r + 1 decode. The driver allows total_out == max_ratio * total_in.
  # identity is excluded: its ratio is 1, and r - 1 = 0 means "no limit".
  for (codec in setdiff(available_codecs(), "identity")) {
    for (n in 20000:20600) {
      z <- komp_compress(raw(n), codec)
      if (n %% length(z) == 0L) break
    }
    expect_identical(n %% length(z), 0L, info = codec)
    r <- n %/% length(z)
    expect_gt(r, 1L)
    expect_identical(decode_outcome(komp_decompress(z, codec, max_ratio = r - 1)),
                     "zukomp_ratio_limit", info = codec)
    expect_identical(decode_outcome(komp_decompress(z, codec, max_ratio = r)),
                     raw(n), info = codec)
    expect_identical(decode_outcome(komp_decompress(z, codec, max_ratio = r + 1)),
                     raw(n), info = codec)
    # The whole stream in one call agrees. Below the final ratio every
    # chunking refuses, since the final totals alone exceed it; at r a
    # finer chunking may refuse earlier, which the #80 test below pins.
    expect_identical(
      decode_outcome(zu_test_stream(z, codec, "decode", in_chunk = length(z),
                                    out_chunk = 65536L, max_ratio = r)),
      raw(n), info = codec)
    expect_identical(
      sweep_mismatches(z, codec, "zukomp_ratio_limit", limit_in_chunks(z),
                       c(7L, 4096L, 65536L), max_ratio = r - 1),
      character(0), info = codec)
  }
  # identity at its own ratio, 1, decodes at every chunking.
  x <- new_payload("ascii", 300L)
  for (k in c(1L, 7L, 300L)) {
    expect_identical(zu_test_stream(x, "identity", "decode", in_chunk = k,
                                    max_ratio = 1), x)
  }
})

test_that("zstd's window ceiling straddles 2^23 and the window max_output implies", {
  # codec_zstd.c caps the window a frame may declare at the power of two at
  # or above max_output, never below 2^23 (design 20). A Window_Descriptor
  # of e << 3 is exactly 2^(10 + e); e << 3 | 1 is the next window up,
  # 2^(10 + e) * 9 / 8 -- no window lies between them.
  w23 <- zstd_window_frame(13L * 8L)
  w23_up <- zstd_window_frame(13L * 8L + 1L)
  data <- charToRaw("window")
  # At the floor: a tiny max_output still admits a 2^23 window, not more.
  expect_identical(komp_decompress(w23, "zstd", max_output = 100), data)
  expect_codec_error(komp_decompress(w23_up, "zstd", max_output = 100),
                     "zukomp_output_limit")
  expect_identical(komp_decompress(w23_up, "zstd", max_output = 0), data)
  for (k in c(1L, 3L, 7L)) {
    expect_identical(
      decode_outcome(zu_test_stream(w23_up, "zstd", "decode", in_chunk = k,
                                    max_output = 100)),
      "zukomp_output_limit", info = paste("in_chunk", k))
  }
  skip_if_no_slow_tests()
  # Derived from max_output, where the rounding moves (these allocate 16 to
  # 18 MiB windows): max_output = 2^23 admits only a 2^23 window, 2^23 + 1
  # rounds up to 2^24, and 2^24 + 1 admits the next window above 2^24.
  w24 <- zstd_window_frame(14L * 8L)
  w24_up <- zstd_window_frame(14L * 8L + 1L)
  expect_codec_error(komp_decompress(w24, "zstd", max_output = 2^23),
                     "zukomp_output_limit")
  expect_identical(komp_decompress(w24, "zstd", max_output = 2^23 + 1), data)
  expect_identical(komp_decompress(w24, "zstd", max_output = 2^24), data)
  expect_codec_error(komp_decompress(w24_up, "zstd", max_output = 2^24),
                     "zukomp_output_limit")
  expect_identical(komp_decompress(w24_up, "zstd", max_output = 2^24 + 1), data)
  # No limit: libzstd's own 2^27 ceiling (a 128 MiB window), past which the
  # frame is unsupported rather than over a limit nobody set.
  expect_identical(komp_decompress(zstd_window_frame(17L * 8L), "zstd",
                                   max_output = 0), data)
  expect_codec_error(komp_decompress(zstd_window_frame(17L * 8L + 1L), "zstd",
                                     max_output = 0),
                     "zukomp_unsupported_codec")
})

test_that("every codec's level range straddles its ends, read from komp_codecs()", {
  # level_min - 1 and level_max + 1 are refused, as zukomp_invalid_argument,
  # both by the R validation in komp_compress() and by the C core when the
  # level reaches it directly (zu_test_stream() skips the R check); the ends
  # themselves compress and round-trip. Read from the table, not literals,
  # so a codec whose range moves is still straddled.
  x <- new_payload("ascii", 2000L)
  tab <- komp_codecs()
  tab <- tab[tab$available & !is.na(tab$level_min), ]
  expect_gt(nrow(tab), 0L)
  for (i in seq_len(nrow(tab))) {
    codec <- tab$id[i]
    lo <- tab$level_min[i]
    hi <- tab$level_max[i]
    for (lv in c(lo, hi)) {
      z <- komp_compress(x, codec, level = lv)
      expect_identical(komp_decompress(z, codec), x,
                       info = paste(codec, "level", lv))
      expect_identical(zu_test_stream(x, codec, "encode", level = lv), z,
                       info = paste(codec, "C level", lv))
    }
    for (lv in c(lo - 1, hi + 1)) {
      expect_codec_error(komp_compress(x, codec, level = lv),
                         "zukomp_invalid_argument")
      expect_codec_error(zu_test_stream(x, codec, "encode", level = lv),
                         "zukomp_invalid_argument")
    }
  }
  # A codec with no level axis takes only NULL (or 0, the default).
  expect_codec_error(komp_compress(x, "identity", level = 1),
                     "zukomp_invalid_argument")
})
