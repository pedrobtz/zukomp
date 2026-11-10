# Steps for zu_test_sequence(), the scripted-handle harness behind
# test-lifecycle.R. Each returns list(op, a, b, c); see zukomp_test_sequence()
# in src/zukomp_test.c for the encoding.

flush_code <- function(flush) {
  switch(flush, run = 0L, flush = 1L, finish = 2L,
         stop("unknown flush: ", flush))
}

# Offer the next `n_in` input bytes and `room` bytes of output space.
step_process <- function(n_in, room = 1e6, flush = "run") {
  list(1L, as.integer(n_in), as.integer(room), flush_code(flush))
}
step_finish <- function(n_in = 0, room = 1e6) {
  step_process(n_in, room, "finish")
}
# Reset under the creation options, and move the input cursor to `cursor`.
step_reset <- function(cursor = 0) list(2L, as.integer(cursor), 0L, 0L)
# The next process() that reaches the codec returns `status` instead.
step_inject <- function(status) {
  list(3L, zu_status_codes()[[status]], 0L, 0L)
}
step_free <- function() list(4L, 0L, 0L, 0L)
step_nullbuf <- function(flush = "run") list(5L, 0L, 0L, flush_code(flush))

# The codecs this build implements, in komp_codecs() order.
lifecycle_codecs <- function() {
  tab <- komp_codecs()
  tab$id[tab$available]
}

lifecycle_can_flush <- function(codec) {
  tab <- komp_codecs()
  isTRUE(tab$can_flush[tab$id == codec])
}

# Asserts the post-error contract (design 9) on steps `from`..`to` of a
# sequence result: every call returns `status`, and none of them consumes
# input or delivers output.
expect_sticky <- function(res, from, status, to = length(res$status),
                          info = NULL) {
  idx <- seq.int(from, to)
  idx <- idx[!is.na(res$status[idx])]
  testthat::expect_identical(res$status[idx], rep(status, length(idx)),
                             info = info)
  testthat::expect_identical(res$consumed[idx], rep(0, length(idx)),
                             info = info)
  testthat::expect_identical(res$produced[idx], rep(0, length(idx)),
                             info = info)
}
