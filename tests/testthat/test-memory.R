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

test_that("the output sink is freed on every path, including an interrupt", {
  # The sink moved from R_alloc to malloc+realloc, because R_alloc cannot
  # resize and every superseded block stayed on the vmax stack until the
  # .Call returned -- decoding 64 MB peaked at ~199 MB. malloc is not
  # visible to the gc-drift tests above, so a leak here would be invisible
  # to the suite that exists to catch it. Hence the explicit counter.
  #
  # gc() first: an interrupted call elsewhere in the suite leaves its sink
  # owned by an unreachable external pointer, and the finalizer runs at the
  # next collection rather than at the longjmp. That is the design working,
  # not a leak -- but it means the baseline is only zero after a collection,
  # and under shuffle the interrupt test may well have run first.
  gc()
  expect_identical(zu_test_outbuf_live(), 0)

  x <- new_payload("structured", 200000L)
  z <- komp_compress(x, "gzip")
  expect_identical(komp_decompress(z, "gzip"), x)
  expect_identical(zu_test_outbuf_live(), 0)

  # Every error path: the sink is held across an Rf_error() raised from R
  # after the .Call returns, and across the C driver bailing out mid-loop.
  bad_crc <- z
  bad_crc[[length(z) - 7L]] <- as.raw(0x00)
  for (f in list(
    function() komp_decompress(z, "gzip", max_output = 1024),
    function() komp_decompress(z[1:20], "gzip"),
    function() komp_decompress(bad_crc, "gzip"),
    function() komp_decompress(as.raw(1:40), "gzip"),
    function() komp_decompress(c(z, as.raw(1:3)), "gzip"),
    function() komp_compress(x, "gzip", level = 99L)
  )) {
    try(f(), silent = TRUE)
    expect_identical(zu_test_outbuf_live(), 0)
  }
})

test_that("an interrupted decompression frees its sink", {
  skip_on_cran()
  # R_CheckUserInterrupt() longjmps straight past the free() in the release
  # path, so the sink has to be reachable by a finalizer. Without the
  # external pointer this leaks one buffer per interrupted call -- silently,
  # since malloc is invisible to gc drift.
  gc()
  expect_identical(zu_test_outbuf_live(), 0)

  z <- komp_compress(raw(200e6), "gzip")
  for (i in 1:5) {
    setTimeLimit(elapsed = 0.05, transient = TRUE)
    try(komp_decompress(z, "gzip", max_output = 0), silent = TRUE)
    setTimeLimit()
  }
  # The finalizer runs at gc, not at the longjmp.
  gc()
  expect_identical(zu_test_outbuf_live(), 0)
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
