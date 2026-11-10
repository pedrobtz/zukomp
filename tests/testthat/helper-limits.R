# Helpers for the limit straddles in test-limit-edges.R.

deflate_codecs <- function() c("deflate-raw", "zlib", "gzip")

# TRUE when the exhaustive sweeps are on. A logical rather than a skip, so a
# test can run its sample on CRAN and widen it in CI instead of skipping.
slow_tests_on <- function() {
  identical(Sys.getenv("ZUKOMP_SLOW_TESTS"), "true") &&
    identical(Sys.getenv("NOT_CRAN"), "true")
}

# The decoded bytes, or the class of the zukomp condition raised instead. A
# sweep compares outcomes rather than stopping at the first error, so one
# run reports every chunking that fails, and two paths can be compared
# directly (the differential pairs in test-limit-edges.R).
decode_outcome <- function(expr) {
  tryCatch(expr, zukomp_error = function(e) class(e)[[1L]])
}

available_codecs <- function() komp_codecs()$id[komp_codecs()$available]

# The input chunkings a straddle runs at: the shared chunk_sizes() sample
# plus the two that end one byte short of the stream and exactly at it. In
# CI (ZUKOMP_SLOW_TESTS) every chunking from 1 to the stream length.
limit_in_chunks <- function(z, all = slow_tests_on()) {
  n <- length(z)
  ks <- if (all) seq_len(n) else c(chunk_sizes(), 100L, n - 1L, n)
  sort(unique(pmin(pmax(as.integer(ks), 1L), n)))
}

# One straddle row per limit and offset: the limit's value relative to the
# decoded size n, and the outcome it must give. NULL means "the payload".
# Crossing max_output is the policy refusing the stream; crossing the
# expected size, in either direction, is the data disagreeing with what the
# caller was told (design 8, decision 19).
limit_straddle <- function(n) {
  list(
    list(arg = "max_output", value = n - 1, want = "zukomp_output_limit"),
    list(arg = "max_output", value = n, want = NULL),
    list(arg = "max_output", value = n + 1, want = NULL),
    list(arg = "expected_size", value = n - 1, want = "zukomp_invalid_data"),
    list(arg = "expected_size", value = n, want = NULL),
    list(arg = "expected_size", value = n + 1, want = "zukomp_invalid_data")
  )
}

# The whole-buffer answer for one straddle row, through the public path that
# carries the limit: komp_decompress() for max_output, the one-shot C ABI
# (zu_decompress_one(), into a sink one byte larger than the payload) for
# the expected size, which komp_decompress() does not take.
whole_outcome <- function(z, codec, row, n) {
  decode_outcome(
    if (row$arg == "max_output") {
      komp_decompress(z, codec, max_output = row$value)
    } else {
      zu_test_decompress_one(z, codec, n + 1, expected_size = row$value)
    }
  )
}

# A zstd frame holding `data` in one raw block, with no content size and no
# checksum, declaring the window `wd` (RFC 8878 3.1.1.1.2): windowLog is
# 10 + (wd >> 3), plus (wd & 7) eighths of that. Without Single_Segment the
# decoder must honour the declared window, which is what windowLogMax caps.
zstd_window_frame <- function(wd, data = charToRaw("window")) {
  bh <- 1 + length(data) * 8          # Last_Block = 1, Block_Type = Raw
  c(as.raw(c(0x28, 0xb5, 0x2f, 0xfd, 0x00, wd)),
    as.raw(c(bh %% 256, (bh %/% 256) %% 256, bh %/% 65536)),
    data)
}

# Decodes `z` at every (in_chunk, out_chunk) pair and names each pair whose
# outcome is not `want`; character(0) when all agree. One expectation per
# sweep keeps a sweep of a thousand calls cheap and lists every failing
# chunking in a single failure message. With `max_delivered`, a pair that
# errs having handed the caller more bytes than that is a mismatch too: a
# limit that refuses the stream after writing past itself has still been
# crossed ("what the caller got before the error", correctness.md). `...`
# is passed to zu_test_stream().
sweep_mismatches <- function(z, codec, want, in_chunks, out_chunks, ...,
                             max_delivered = NULL) {
  bad <- character()
  for (k in in_chunks) {
    for (m in out_chunks) {
      got <- tryCatch(
        zu_test_stream(z, codec, "decode", in_chunk = k, out_chunk = m, ...),
        zukomp_error = identity)
      delivered <- NA_real_
      if (inherits(got, "zukomp_error")) {
        delivered <- got$output_bytes
        got <- class(got)[[1L]]
      }
      what <- if (is.raw(got)) sprintf("%d bytes", length(got)) else got
      if (!identical(got, want)) {
        bad <- c(bad, sprintf("in=%d out=%d: %s", k, m, what))
      } else if (!is.null(max_delivered) && !is.na(delivered) &&
                 delivered > max_delivered) {
        bad <- c(bad, sprintf("in=%d out=%d: %s after %d bytes", k, m, what,
                              as.integer(delivered)))
      }
    }
  }
  bad
}
