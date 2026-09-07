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
    utf8       = charToRaw(enc2utf8(substr(strrep("café 中文 über ", n), 1L, n))),
    random     = as.raw(sample.int(256L, n, replace = TRUE) - 1L),
    structured = charToRaw(substr(
      paste0('{"id":', seq_len(max(1L, n %/% 16L)), ',"v":"x"}', collapse = ","),
      1L, n
    )),
    stop("unknown payload kind: ", kind)
  )
}

payload_kinds <- function() {
  c("empty", "one_byte", "zeros", "ones", "ascii", "utf8", "random", "structured")
}

# Chunk sizes worth sweeping. 1 is the harshest boundary, the small primes
# catch off-by-one in cursor arithmetic, and 32/4096 are ordinary cases.
chunk_sizes <- function() c(1L, 2L, 3L, 7L, 31L, 32L, 4096L)
