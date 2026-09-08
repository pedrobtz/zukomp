# Exhaustive sweeps are the difference between "we sampled the truncation
# positions" and "every truncation position of every stream errors". CRAN
# gets the sample so the suite stays under a minute; CI runs the real thing.
skip_if_no_slow_tests <- function() {
  skip_on_cran()
  if (!identical(Sys.getenv("ZUKOMP_SLOW_TESTS"), "true")) {
    skip("set ZUKOMP_SLOW_TESTS=true to run exhaustive sweeps")
  }
}
