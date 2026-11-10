# Trailing bytes after a stream that ends exactly where an input slice ends
# (#90).
#
# zu_int_run_whole() offers its input a slice at a time -- 64 KiB from
# komp_decompress(), in_chunk from the harness -- and a decoder can only
# report trailing bytes it was shown. A stream that ended on the last byte of
# a slice returned ZU_STREAM_END with nothing left in that slice, and the
# loop stopped without looking at the slices it never offered: trailing junk
# and a second member under concat_members = FALSE were dropped silently,
# though ZU_DEC_REJECT_TRAILING was set.

test_that("junk after a stream is rejected wherever the slices fall", {
  x <- new_payload("ascii", 3000)
  junk <- charToRaw("junk")
  for (codec in c("deflate-raw", "zlib", "gzip", "zstd", "lz4-frame")) {
    y <- komp_compress(x, codec)
    n <- length(y)
    for (in_chunk in c(1, n - 1, n, n + 1)) {
      expect_error(
        zu_test_stream(c(y, junk), codec, "decode", in_chunk = in_chunk),
        class = "zukomp_trailing_bytes",
        info = paste(codec, in_chunk)
      )
      # Not rejecting is still exact: the stream, and the cursor at its end.
      got <- zu_test_stream(c(y, junk), codec, "decode", in_chunk = in_chunk,
                            reject_trailing = FALSE, report_consumed = TRUE)
      expect_identical(as.vector(got), x, info = paste(codec, in_chunk))
      expect_identical(attr(got, "consumed"), as.double(n),
                       info = paste(codec, in_chunk))
    }
  }
})

test_that("a second member is trailing data when members are not joined", {
  x <- new_payload("ascii", 3000)
  for (codec in c("gzip", "zstd", "lz4-frame")) {
    y <- komp_compress(x, codec)
    n <- length(y)
    for (in_chunk in c(1, 2, n - 1, n, n + 1)) {
      expect_error(
        zu_test_stream(c(y, y), codec, "decode", in_chunk = in_chunk,
                       concat_members = FALSE),
        class = "zukomp_trailing_bytes",
        info = paste(codec, in_chunk)
      )
      expect_identical(
        zu_test_stream(c(y, y), codec, "decode", in_chunk = in_chunk),
        c(x, x),
        info = paste(codec, in_chunk)
      )
    }
  }
})

test_that("komp_decompress() rejects junk after a stream of 64 KiB +- 1", {
  # komp_decompress() offers 64 KiB slices, so a compressed stream of
  # exactly 65536 bytes ends on a slice boundary. Stored blocks make the
  # compressed size a known function of the input size; search for the
  # three sizes rather than hard-coding miniz's block length.
  base <- new_payload("lcg", 66000)
  size_of <- function(m) length(komp_compress(base[seq_len(m)], "deflate-raw", level = 0))
  found <- list()
  for (m in 65400:65600) {
    k <- size_of(m)
    if (k %in% c(65535, 65536, 65537) && is.null(found[[as.character(k)]])) {
      found[[as.character(k)]] <- m
    }
  }
  expect_named(found, c("65535", "65536", "65537"), ignore.order = TRUE)
  for (k in names(found)) {
    x <- base[seq_len(found[[k]])]
    y <- komp_compress(x, "deflate-raw", level = 0)
    expect_length(y, as.integer(k))
    expect_identical(komp_decompress(y, "deflate-raw"), x)
    expect_error(
      komp_decompress(c(y, charToRaw("junk")), "deflate-raw"),
      class = "zukomp_trailing_bytes",
      info = k
    )
  }
})
