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

test_that("a zstd frame's ceiling is on min(window, declared content size)", {
  # libzstd's streaming buffer is the smaller of the window and the declared
  # content size, but it refused on the window alone -- except on its
  # single-pass path, taken when the output room covered the content size.
  # So this valid frame decoded with 300 bytes of room and was refused as
  # over the limit with 299 (#119). Frame header 0x40: a two-byte
  # Frame_Content_Size (value + 256), Single_Segment_Flag clear; 0x80 is a
  # 2^26 window, above the 2^23 ceiling max_output = 1000 implies.
  data <- as.raw(seq_len(300L) %% 251L)
  frame <- function(fhd, wd, fcs_bytes) {
    bh <- 1 + length(data) * 8          # Last_Block = 1, Block_Type = Raw
    c(as.raw(c(0x28, 0xb5, 0x2f, 0xfd, fhd, wd)), fcs_bytes,
      as.raw(c(bh %% 256, (bh %/% 256) %% 256, bh %/% 65536)), data)
  }
  z <- frame(0x40, 0x80, as.raw(c(300L - 256L, 0L)))
  for (cap in list(1000, NULL)) {
    for (oc in c(1L, 7L, 299L, 300L, 1e5L)) {
      for (ic in c(1L, length(z))) {
        expect_identical(
          zu_test_stream(z, "zstd", "decode", in_chunk = ic, out_chunk = oc,
                         max_output = cap),
          data, info = paste("max_output", format(cap), "out", oc, "in", ic))
      }
    }
  }
  # With no limit too, a declared size admits a window past libzstd's 2^27
  # (0xA8: 2^31), since the buffer is the content size.
  expect_identical(komp_decompress(frame(0x40, 0xA8, as.raw(c(44L, 0L))),
                                   "zstd", max_output = 0), data)
  # Controls: the same window with no content size declared, and with one
  # above the ceiling (an eight-byte field, FHD 0xC0, declaring 2^24), is
  # still refused before a byte is decoded, at any output room.
  no_fcs <- frame(0x00, 0x80, raw(0))
  big_fcs <- frame(0xC0, 0x80, as.raw(c(0, 0, 0, 1, 0, 0, 0, 0)))
  for (oc in c(7L, 1e5L)) {
    for (bad in list(no_fcs, big_fcs)) {
      expect_identical(
        decode_outcome(zu_test_stream(bad, "zstd", "decode",
                                      in_chunk = length(bad), out_chunk = oc,
                                      max_output = 1000)),
        "zukomp_output_limit", info = paste("out", oc))
    }
  }
  expect_identical(komp_decompress(no_fcs, "zstd", max_output = 2^26), data)
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

test_that("max_ratio bounds expansion as decoding proceeds, not the final ratio", {
  # #80, design 22 decision 22. The driver checks the ratio after every
  # process() call, on the bytes decoded and consumed so far. So a stream
  # with a highly compressible prefix can be refused although its overall
  # ratio is under the limit, and whether it is depends on how much input
  # the codec takes per call -- the input slices and the output window. This
  # pins that decision from both sides, at several windows, with a payload
  # whose halves differ: 200 KB of zeros (about 1000:1) then 200 KB of
  # incompressible bytes, for a final ratio near 2.
  x <- c(raw(200000L), lcg_bytes(200000L))
  outs <- c(4096L, 65536L, 1000000L)
  for (codec in setdiff(available_codecs(), "identity")) {
    z <- komp_compress(x, codec)
    ins <- c(4096L, 65536L, length(z))
    expect_lt(length(x) / length(z), 3, label = codec)
    # Below the final ratio, every delivery refuses: the totals alone exceed
    # it. Above the largest ratio any prefix can reach (all output from one
    # byte), none does. Both hold under either reading of the limit.
    expect_identical(
      sweep_mismatches(z, codec, "zukomp_ratio_limit", ins, outs, max_ratio = 1),
      character(0), info = paste(codec, "max_ratio = 1"))
    expect_identical(
      sweep_mismatches(z, codec, x, ins, outs, max_ratio = length(x)),
      character(0), info = paste(codec, "max_ratio = length(x)"))
    # Between them the reading decides. Offered the whole stream with room
    # for the whole output, a codec decodes in one call and is judged on the
    # final ratio, so it decodes. lz4-block gathers all of its input before
    # producing anything, so it is always judged that way.
    expect_identical(
      sweep_mismatches(z, codec, x, length(z), 1000000L, max_ratio = 3),
      character(0), info = paste(codec, "max_ratio = 3, one call"))
    if (codec == "lz4-block") {
      expect_identical(decode_outcome(komp_decompress(z, codec, max_ratio = 3)),
                       x)
      expect_identical(
        sweep_mismatches(z, codec, x, ins, outs, max_ratio = 3),
        character(0), info = paste(codec, "max_ratio = 3"))
      next
    }
    # Every other codec decodes the zeros before it has consumed the second
    # half, so any smaller slice or window is refused -- komp_decompress()'s
    # 64 KiB slices included.
    expect_identical(decode_outcome(komp_decompress(z, codec, max_ratio = 3)),
                     "zukomp_ratio_limit", info = codec)
    for (k in ins) {
      for (m in outs) {
        if (k == length(z) && m == 1000000L) next
        expect_identical(
          sweep_mismatches(z, codec, "zukomp_ratio_limit", k, m, max_ratio = 3),
          character(0), info = paste(codec, "max_ratio = 3"))
      }
    }
  }
})

test_that("the DEFLATE family crosses its miniz slice without truncating", {
  # #73. codec_deflate.c hands miniz at most 2^31 - 1 bytes per call, and
  # passed MZ_FINISH with the first slice of a larger input: the encoder
  # ended the stream there, with a trailer over that prefix, so the output
  # was a valid stream of the first 2^31 - 1 bytes and nothing could tell.
  # The slice is lowered to 1000 bytes so inputs straddle it cheaply: one
  # short, at, one over, and over two slices. The oracle is the output with
  # the real slice, byte for byte, plus gzip's ISIZE and a round trip
  # through the decoder, which slices its input the same way.
  sizes <- c(999L, 1000L, 1001L, 2001L, 5000L)
  xs <- lapply(sizes, function(n) new_payload("ascii", n))
  real <- lapply(deflate_codecs(), function(codec) {
    lapply(xs, komp_compress, codec = codec)
  })
  names(real) <- deflate_codecs()
  local_deflate_slice(1000)
  for (codec in deflate_codecs()) {
    for (i in seq_along(sizes)) {
      x <- xs[[i]]
      info <- sprintf("%s n=%d", codec, sizes[[i]])
      z <- komp_compress(x, codec)
      expect_identical(z, real[[codec]][[i]], info = info)
      expect_identical(as.vector(zu_test_compress_one(x, codec)), z, info = info)
      expect_identical(zu_test_stream(x, codec, "encode",
                                      in_chunk = length(x)), z, info = info)
      if (codec == "gzip") {
        expect_identical(gzip_isize(z), as.double(length(x)), info = info)
      }
      expect_identical(komp_decompress(z, codec), x, info = info)
    }
  }
})

test_that("the DEFLATE family compresses past 2^31 - 1 bytes in one call", {
  # #73 at the real slice: n = 2^31 - 3 to 2^31 + 2 zeros, through
  # komp_compress() and the one-shot ABI. Checked without decoding 2 GiB:
  # gzip's ISIZE is n mod 2^32, the Adler-32 of n zeros is
  # (n mod 65521) * 2^16 + 1, and the three wrappers carry the same body.
  skip_if_no_huge_tests()
  for (n in 2^31 + (-3:2)) {
    x <- raw(n)
    gz <- komp_compress(x, "gzip", level = 1)
    expect_identical(gzip_isize(gz), n, info = n)
    expect_identical(as.vector(zu_test_compress_one(x, "gzip", level = 1)), gz,
                     info = n)
    zl <- komp_compress(x, "zlib", level = 1)
    adler <- sum(as.integer(zl[length(zl) - 3:0]) * 256^(3:0))
    expect_identical(adler, (n %% 65521) * 65536 + 1, info = n)
    raw_body <- komp_compress(x, "deflate-raw", level = 1)
    expect_identical(zl[3:(length(zl) - 4L)], raw_body, info = n)
    expect_identical(gz[11:(length(gz) - 8L)], raw_body, info = n)
    rm(x, gz, zl, raw_body)
    gc()
  }
})
