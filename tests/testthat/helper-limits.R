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
