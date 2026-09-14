# The compress half of the one-shot C ABI: zu_compress_bound() and
# zu_compress_one().
#
# Until now these ran only against the consumer package's xor5a -- a codec
# with no wrapper, no expansion and bound(n) == n, which is precisely the
# one shape that cannot catch a bound that forgets a header, a trailer or
# stored-block overhead. gzip and zlib have all three.

test_that("the bound holds for every codec and every payload kind", {
  for (codec in c("identity", "deflate-raw", "zlib", "gzip")) {
    for (kind in payload_kinds()) {
      for (n in c(0L, 1L, 1000L)) {
        x <- new_payload(kind, n)
        z <- zu_test_compress_one(x, codec, cap_delta = 0)
        expect_lte(length(z), attr(z, "bound"))
        expect_identical(komp_decompress(z, codec), x,
                         info = paste(codec, kind, n))
      }
    }
  }
})

test_that("the bound holds where it is closest to wrong", {
  # Incompressible input is the worst case: DEFLATE falls back to stored
  # blocks, so the output is the input plus per-block overhead plus the
  # wrapper. A bound that forgot the gzip header and trailer fails here and
  # nowhere else.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    for (n in c(1L, 2L, 255L, 256L, 65535L, 65536L, 70000L)) {
      x <- new_payload("lcg", n)
      z <- zu_test_compress_one(x, codec, cap_delta = 0)
      expect_lte(length(z), attr(z, "bound"))
      expect_identical(komp_decompress(z, codec), x,
                       info = paste(codec, n))
    }
  }
})

test_that("the bound covers every level, not just the default", {
  # zlib_bound() and gzip_bound() ignore the level argument and size for the
  # default. Level 0 is stored blocks, which is the largest output any level
  # produces, so that is the level the bound has to cover.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    for (level in 0:9) {
      x <- new_payload("lcg", 40000)
      z <- zu_test_compress_one(x, codec, level = level, cap_delta = 0)
      expect_lte(length(z), attr(z, "bound"))
      expect_identical(komp_decompress(z, codec), x,
                       info = paste(codec, level))
    }
  }
})

test_that("a capacity below what is needed is an output-limit error", {
  # Not a data error and not a crash: the one-shot functions allocate
  # nothing, so too small a buffer has to come back as ZU_ERR_OUTPUT_LIMIT.
  for (codec in c("deflate-raw", "zlib", "gzip", "identity")) {
    x <- new_payload("lcg", 5000)
    needed <- length(zu_test_compress_one(x, codec, cap_delta = 0))
    expect_error(
      zu_test_compress_one(x, codec, cap_delta = -(attr(
        zu_test_compress_one(x, codec, cap_delta = 0), "bound") - needed + 1)),
      class = "zukomp_output_limit", info = codec
    )
  }
})

test_that("a zero-capacity sink is an output-limit error, not a crash", {
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    x <- new_payload("ascii", 1000)
    b <- zu_test_compress_bound(x, codec)
    expect_error(zu_test_compress_one(x, codec, cap_delta = -b),
                 class = "zukomp_output_limit", info = codec)
  }
})

test_that("empty input compresses into the bound", {
  # An empty payload still costs a wrapper: gzip's is 18 bytes of header and
  # trailer with nothing between them, so bound(0) must not be 0.
  for (codec in c("identity", "deflate-raw", "zlib", "gzip")) {
    x <- raw(0)
    z <- zu_test_compress_one(x, codec, cap_delta = 0)
    expect_lte(length(z), attr(z, "bound"))
    expect_identical(komp_decompress(z, codec), x, info = codec)
  }
})

test_that("one-shot output is identical to the streaming path", {
  # There is no second code path for compression either: whatever the
  # one-shot writes must be byte-for-byte what komp_compress() produces.
  for (codec in c("identity", "deflate-raw", "zlib", "gzip")) {
    for (kind in c("ascii", "utf8", "structured", "lcg")) {
      x <- new_payload(kind, 3000)
      expect_identical(
        as.raw(zu_test_compress_one(x, codec, cap_delta = 0)),
        komp_compress(x, codec),
        info = paste(codec, kind)
      )
    }
  }
})
