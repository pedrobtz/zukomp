# The two payload constructors from tools/make-link-fixture.R, repeated here
# character-for-character. They are repeated rather than shared because this
# is a separate package: it cannot reach zukomp's helper-corpus.R, and it
# must keep working with zukomp absent from the library path entirely, which
# is what tools/check-linking.sh checks by moving the installed zukomp away.
#
# The generator's --check mode is what keeps the two copies honest: it
# rebuilds probe.zip and refuses to differ from what is committed.

payload_text <- function() {
  charToRaw(paste(rep("zukomp reads ZIP containers\n", 180L), collapse = ""))
}

payload_lcg <- function(n) {
  x <- 1L
  out <- raw(n)
  for (i in seq_len(n)) {
    x <- (75 * x + 74) %% 65537
    out[i] <- as.raw(x %% 256)
  }
  out
}

probe_zip <- function() {
  path <- system.file("extdata", "probe.zip", package = "zukomplink")
  skip_if(!nzchar(path) || !file.exists(path), "probe.zip is not installed")
  path
}

# 1 is the harshest boundary, as it is in zukomp's own sweeps.
chunk_sizes <- function() c(1L, 2L, 3L, 7L, 31L, 64L, 4096L, 0L)
