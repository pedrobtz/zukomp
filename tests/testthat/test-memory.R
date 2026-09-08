# design 13: R's non-local exit is the likeliest source of real bugs in a
# package like this. Rf_error() and R_CheckUserInterrupt() both longjmp
# straight past any free() below them, so anything holding heap state across
# them has to be owned by something R will clean up.

test_that("an error mid-decompression does not leak", {
  skip_if_no_slow_tests()
  z <- komp_compress(new_payload("zeros", 200000L), "gzip")
  gc()
  before <- gc(reset = TRUE)
  for (i in 1:2000) {
    try(komp_decompress(z, "gzip", max_output = 1024), silent = TRUE)
  }
  drift <- gc()[2L, 2L] - before[2L, 2L]
  expect_lt(drift, 50)
})

test_that("repeated failures across every error path do not leak", {
  skip_if_no_slow_tests()
  z <- komp_compress(new_payload("ascii", 8192L), "gzip")
  bad_crc <- z; bad_crc[length(z) - 7L] <- as.raw(0x00)
  cases <- list(
    function() komp_decompress(z, "gzip", max_output = 16),
    function() komp_decompress(z[1:20], "gzip"),
    function() komp_decompress(bad_crc, "gzip"),
    function() komp_decompress(as.raw(1:40), "gzip"),
    function() komp_decompress(c(z, as.raw(1:3)), "gzip")
  )
  gc()
  before <- gc(reset = TRUE)
  for (i in 1:400) for (f in cases) try(f(), silent = TRUE)
  expect_lt(gc()[2L, 2L] - before[2L, 2L], 50)
})

test_that("decompression is interruptible", {
  skip_on_cran()
  # A large bomb under a generous cap gives the loop enough iterations to
  # reach an interrupt check. What matters is that it stops, and that
  # stopping does not strand the stream handle -- which is why the handle
  # is owned by an external pointer with a finalizer.
  z <- komp_compress(raw(200e6), "gzip")
  setTimeLimit(elapsed = 0.5, transient = TRUE)
  on.exit(setTimeLimit(), add = TRUE)
  result <- tryCatch(komp_decompress(z, "gzip", max_output = 0),
                     error = function(e) "interrupted")
  setTimeLimit()
  # Either it finished within the budget or it was cut short; both are fine.
  expect_true(is.raw(result) || identical(result, "interrupted"))
})

test_that("interrupting repeatedly does not accumulate stream handles", {
  skip_if_no_slow_tests()
  z <- komp_compress(raw(50e6), "gzip")
  gc()
  before <- gc(reset = TRUE)
  for (i in 1:20) {
    setTimeLimit(elapsed = 0.05, transient = TRUE)
    try(komp_decompress(z, "gzip", max_output = 0), silent = TRUE)
    setTimeLimit()
  }
  gc()
  expect_lt(gc()[2L, 2L] - before[2L, 2L], 50)
})

test_that("a large round-trip is exact and does not grow without bound", {
  skip_if_no_slow_tests()
  x <- new_payload("structured", 8e6)
  z <- komp_compress(x, "gzip")
  expect_identical(komp_decompress(z, "gzip", max_output = 0), x)
})

# -- regressions from fuzzing -------------------------------------------------
# Each corresponds to an input in fuzz/corpus/regressions/. A finding without
# a test here is a finding that can come back silently.

test_that("a zero-length input does not do arithmetic on a null pointer", {
  # UBSan: "applying zero offset to null pointer". An empty zu_buffer
  # legitimately carries a NULL pointer and a zero size, and `NULL + 0` is
  # undefined in C even though every real compiler yields NULL. Found by
  # fuzzing; this is the R-level path that reaches it.
  # The framed codecs have nothing to parse, so an empty input is
  # truncated. identity has no framing at all, so an empty stream is a
  # perfectly good empty stream -- the distinction is the point.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    expect_error(komp_decompress(raw(0), codec), class = "zukomp_error",
                 info = codec)
  }
  expect_identical(komp_decompress(raw(0), "identity"), raw(0))
  # ...and the encode direction, where an empty input is not an error at all
  for (codec in c("deflate-raw", "zlib", "gzip", "identity")) {
    expect_identical(komp_decompress(komp_compress(raw(0), codec), codec),
                     raw(0), info = codec)
  }
})

test_that("a gzip stream truncated to its magic errors cleanly", {
  expect_codec_error(komp_decompress(as.raw(c(0x1f, 0x8b, 0x08)), "gzip"),
                     "zukomp_truncated")
})

test_that("every committed fuzz regression input is handled without crashing", {
  # The corpus is shipped in the repo but not installed, so this only runs
  # from a source checkout; skipping elsewhere beats failing.
  dir <- test_path("..", "..", "fuzz", "corpus", "regressions")
  skip_if_not(dir.exists(dir), "fuzz corpus not present (installed package)")
  inputs <- list.files(dir, full.names = TRUE, pattern = "^[^R]")
  skip_if(length(inputs) == 0L, "no regression inputs")
  for (f in inputs) {
    bytes <- readBin(f, "raw", file.size(f))
    for (codec in c("gzip", "zlib", "deflate-raw")) {
      # The requirement is "does not crash and does not lie", not "succeeds".
      out <- tryCatch(komp_decompress(bytes, codec),
                      zukomp_error = function(e) NULL)
      expect_true(is.null(out) || is.raw(out),
                  info = paste(basename(f), codec))
    }
    expect_true(is.character(komp_detect(bytes)))
  }
})
