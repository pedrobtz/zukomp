# The allocation-failure sweep's workload (#12; r-actions alloc-failure.yml).
#
# The sweep runs this once per allocation, failing a different one each time,
# R's own included. It is the one check that fails an allocation inside a
# codec's _new or process(), inside the output sink's realloc in
# zu_int_run_whole(), and inside R's own allocations while that sink is held
# -- the design 13 obligation that native memory held across an R allocation
# is owned by an external pointer with a finalizer. Nothing in the testthat
# suite makes malloc fail.
#
# Every codec, whole and streamed: komp_compress()/komp_decompress(), and the
# zu_test_stream() harness at odd chunk sizes, which drives the same loop
# through the codec's own buffering. Every result is checked byte for byte
# against the input: a wrong result exits 3 and prints no condition class,
# so it matches no expect-pattern and the sweep reports it as a silent
# failure. An operation that fails prints its condition class and exits 1.
#
#   Rscript tools/alloc/exercise.R             # the workload
#   Rscript tools/alloc/exercise.R --baseline  # everything up to the first codec call
suppressPackageStartupMessages(library(zukomp))
baseline <- "--baseline" %in% commandArgs(TRUE)

# Force every lazy-loaded binding in the namespace now, in the baseline as in
# the run, so an injected failure after this point lands in zukomp's work and
# not in R decompressing its lazy-load database for a helper the first call
# happens to touch.
ns <- asNamespace("zukomp")
invisible(mget(ls(ns, all.names = TRUE), envir = ns))
stream <- get("zu_test_stream", envir = ns)

# Deterministic and partly compressible, so every codec emits matches and
# literals and the decoders' back-references are exercised. 24 KiB is several
# of each side's chunks and small enough to keep the sweep short.
want <- as.raw(c((0:16383 * 7 + 3) %% 251, (0:8191 * 31) %% 256))
codecs <- c(
  "identity",
  "deflate-raw",
  "zlib",
  "gzip",
  "zstd",
  "lz4-frame",
  "lz4-block"
)

if (baseline) {
  quit(status = 0, save = "no")
}

run <- function(what, expr) {
  got <- tryCatch(expr, error = function(e) e)
  if (inherits(got, "error")) {
    cat(what, ": ", class(got)[1], ": ", conditionMessage(got), "\n", sep = "")
    quit(status = 1, save = "no")
  }
  if (!identical(got, want)) {
    cat(what, ": WRONG RESULT, ", length(got), " bytes\n", sep = "")
    quit(status = 3, save = "no")
  }
}

for (codec in codecs) {
  run(paste(codec, "whole"), {
    komp_decompress(komp_compress(want, codec), codec)
  })
  # Odd, unequal chunk sizes, so neither side's boundaries line up with the
  # other's or with a codec's internal block size.
  run(paste(codec, "streamed"), {
    enc <- stream(want, codec, "encode", in_chunk = 997, out_chunk = 1013)
    stream(enc, codec, "decode", in_chunk = 1009, out_chunk = 991)
  })
}
cat("ok\n")
quit(status = 0, save = "no")
