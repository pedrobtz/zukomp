# Payload constructors rather than stored data: cheap, parameterisable, and
# they keep the tarball small. Each kind stresses a different part of a
# compressor -- all-same bytes, incompressible noise, text with real
# redundancy, multi-byte UTF-8, and the highly structured shape that JSON
# bodies over HTTP actually have.

new_payload <- function(kind, n = 4096L) {
  n <- as.integer(n)
  switch(
    kind,
    empty      = raw(0),
    one_byte   = as.raw(0x7f),
    zeros      = raw(n),
    ones       = as.raw(rep(0xffL, n)),
    ascii      = charToRaw(substr(strrep("the quick brown fox jumps. ", n), 1L, n)),
    utf8       = utf8_bytes(n),
    random     = as.raw(sample.int(256L, n, replace = TRUE) - 1L),
    # Deterministic and barely compressible, so it exercises DEFLATE's
    # stored blocks. Park-Miller, using middle bits: reproducible in R,
    # Python and C alike, unlike sample.int(), which is why the interop
    # fixtures use this kind and not "random".
    lcg        = lcg_bytes(n),
    structured = charToRaw(substr(
      paste0('{"id":', seq_len(max(1L, n %/% 16L)), ',"v":"x"}', collapse = ","),
      1L, n
    )),
    stop("unknown payload kind: ", kind)
  )
}

payload_kinds <- function() {
  c("empty", "one_byte", "zeros", "ones", "ascii", "utf8", "random",
    "structured", "lcg")
}

# "café 中文 über " as explicit UTF-8 bytes, repeated to n and padded with
# spaces so the result is exactly n bytes and still valid UTF-8.
#
# Spelled out byte by byte on purpose. Building it from a string literal
# makes the payload depend on how R decodes this file's encoding, which
# varies by platform and locale -- and a fixture generated on one machine
# then fails to match on another. That is not hypothetical: it is how this
# function was first written, and the gzip interop test caught it.
utf8_bytes <- function(n) {
  n <- as.integer(n)
  if (n <= 0L) return(raw(0))
  unit <- as.raw(c(
    0x63, 0x61, 0x66, 0xc3, 0xa9, 0x20,               # "café "
    0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87, 0x20,         # "中文 "
    0xc3, 0xbc, 0x62, 0x65, 0x72, 0x20                # "über "
  ))
  reps <- n %/% length(unit)
  out <- if (reps > 0L) rep(unit, reps) else raw(0)
  pad <- n - length(out)
  if (pad > 0L) out <- c(out, as.raw(rep(0x20, pad)))
  out
}

# Park-Miller: x <- 16807 * x mod (2^31 - 1). Every intermediate stays under
# 2^53, so double arithmetic is exact and R, Python and C agree byte for byte.
lcg_bytes <- function(n) {
  n <- as.integer(n)
  if (n <= 0L) return(raw(0))
  out <- integer(n)
  x <- 20260907
  for (i in seq_len(n)) {
    x <- (16807 * x) %% 2147483647
    out[i] <- as.integer(x %/% 128) %% 256L
  }
  as.raw(out)
}

# Chunk sizes worth sweeping. 1 is the harshest boundary, the small primes
# catch off-by-one in cursor arithmetic, and 32/4096 are ordinary cases.
chunk_sizes <- function() c(1L, 2L, 3L, 7L, 31L, 32L, 4096L)
