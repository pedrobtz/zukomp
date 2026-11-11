# Runs test-limit-edges.R with the 2 GiB tests on (#101) and fails unless
# every test in it ran: a skip there means the boundary went unchecked, and
# a job that reports success over a skipped test checks nothing. Run from
# the repository root; .github/workflows/huge-tests.yaml calls it.
if (!identical(Sys.getenv("ZUKOMP_HUGE_TESTS"), "true")) {
  stop("ZUKOMP_HUGE_TESTS must be \"true\": without it the 2 GiB tests skip")
}
res <- as.data.frame(testthat::test_local(
  ".", filter = "^limit-edges$", reporter = "progress",
  stop_on_failure = FALSE
))
if (nrow(res) == 0L) {
  stop("no tests ran: the filter matched no file")
}
cat(sprintf("\n%d tests, %d failed, %d errors, %d skipped\n", nrow(res),
            sum(res$failed), sum(res$error), sum(res$skipped)))
if (any(res$skipped)) {
  stop("skipped: ", paste(res$test[res$skipped], collapse = "; "))
}
if (sum(res$failed) > 0L || any(res$error)) {
  quit(status = 1L)
}
