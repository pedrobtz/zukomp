# zukomp.h's thread contract (#133), checked the way zugodb will rely on it:
# the table fetched once on R's main thread with zukomp_api(), then called
# from worker threads, each with decoders and encoders of its own. The
# ThreadSanitizer run of the same body is ctest's `make tsan`.

test_that("zstd and lz4 decode and encode the same on several threads", {
  # Two threads by default, the most CRAN lets a check use;
  # ZUKOMP_TEST_THREADS raises it where that does not apply.
  threads <- as.integer(Sys.getenv("ZUKOMP_TEST_THREADS", "2"))
  rounds <- 3L
  # Four comparisons (two decodes, two encodes) per codec and size, three
  # codecs at four sizes: a count that falls short means a worker stopped.
  expect_identical(decode_on_threads(threads, rounds),
                   as.double(threads * rounds * 12L * 4L))
})

test_that("one thread gives the same answers as several", {
  expect_identical(decode_on_threads(1L, 1L), 48)
})

test_that("a page decodes into a sink of its size and never past it", {
  # zstd and lz4-block, three page sizes, sinks of n - 1, n and n + 1, both
  # one-shot and through a reused decoder.
  expect_identical(page_caps_via_c(), 2 * 3 * 2 * 3)
})
