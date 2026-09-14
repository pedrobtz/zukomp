# Readers for the hand-built conformance corpus in fixtures/malformed/.
#
# Separate from the interop fixture helpers in helper-expect.R because the
# two corpora answer different questions: that one is round-trip shaped and
# entirely valid, this one is conformance shaped and mostly invalid. They
# share almost no columns, so they have separate manifests.

# colClasses = "character" is load bearing. `output_n` and `output_hex` hold
# all-digit strings, and read.delim()'s type conversion would turn "" into NA
# and strip a hex string's leading zeros -- silently, and only for some
# corpora, which is the worst way for a fixture reader to break.
malformed_manifest <- function(expect = NULL) {
  m <- read.delim(test_path("fixtures", "malformed", "MANIFEST.tsv"),
                  colClasses = "character")
  if (!is.null(expect)) m <- m[m$expect %in% expect, , drop = FALSE]
  m
}

malformed_bytes <- function(case) {
  path <- test_path("fixtures", "malformed", paste0(case, ".bin"))
  readBin(path, "raw", file.size(path))
}

# "4142" -> as.raw(c(0x41, 0x42)), "" -> raw(0).
hex_to_raw <- function(hex) {
  if (!nzchar(hex)) return(raw(0))
  as.raw(strtoi(substring(hex, seq(1L, nchar(hex), 2L),
                          seq(2L, nchar(hex), 2L)), 16L))
}

# RFC 1950's two-byte header, and a trailer that is deliberately wrong: the
# point of wrapping a malformed body is to show the checksum catching what
# raw DEFLATE cannot, and the correct Adler-32 of heap garbage is unknowable.
zlib_wrap <- function(body) c(as.raw(c(0x78, 0x01)), body, raw(4))
gzip_wrap <- function(body) {
  c(as.raw(c(0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xff)), body, raw(8))
}
