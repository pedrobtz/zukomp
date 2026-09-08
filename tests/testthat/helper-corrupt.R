# Corruption and truncation helpers. In a helper file, not at the top of a
# test file: testthat's parallel workers source helper-*.R but do not share a
# test file's file-scope definitions.

# Flips every bit of byte `i`, which is a bigger perturbation than a single
# bit and so more likely to be caught -- the point is to prove detection,
# not to explore how subtle a corruption can be.
flip_byte <- function(z, i) {
  z[i] <- as.raw(bitwXor(as.integer(z[i]), 0xff))
  z
}

# Which prefix lengths to test. The leading bytes are the wrapper header,
# where the parser bugs live, so they are always covered; the remainder is
# sampled to keep the CRAN suite inside its budget, and covered exhaustively
# when ZUKOMP_SLOW_TESTS is set.
truncation_positions <- function(n) {
  if (identical(Sys.getenv("ZUKOMP_SLOW_TESTS"), "true")) {
    return(seq_len(n - 1L))
  }
  sort(unique(c(seq_len(min(16L, n - 1L)),
                sample.int(n - 1L, min(32L, n - 1L)))))
}
