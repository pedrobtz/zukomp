# Runs a package's testthat suite and exits non-zero on any failure, error,
# or on no tests at all:
#
#   Rscript tools/ci/run-tests.R <path> [installed]
#
# A file rather than `Rscript -e`: on Windows only the first line of a
# multi-line -e argument reaches R, which then halts on an incomplete
# expression instead of running the tests.
args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1L) stop("usage: run-tests.R <path> [installed]")
path <- args[[1L]]
load <- if (length(args) >= 2L && identical(args[[2L]], "installed")) {
  "installed"
} else {
  "source"
}
results <- as.data.frame(testthat::test_local(
  path, reporter = "summary", load_package = load))
# A zero-row result is a green job that proved nothing: any() over an empty
# vector is FALSE. If test_local() discovers no files -- a rename, an
# .Rbuildignore change, a load failure -- that must fail, not pass quietly.
if (nrow(results) == 0L || sum(results$passed) == 0L) {
  stop("no tests ran in ", path)
}
quit(status = as.integer(any(results$failed > 0 | results$error)))
