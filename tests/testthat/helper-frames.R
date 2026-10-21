# Helpers for the framed codecs, zstd and lz4-frame (test-frames.R).

framed_codecs <- function() c("zstd", "lz4-frame")

# A skippable frame (magic 0x184D2A50, little-endian) carrying `n` bytes of
# user data. Both formats define the same skippable frame.
skippable_frame <- function(n) {
  c(as.raw(c(0x50, 0x2a, 0x4d, 0x18)),
    as.raw(c(n %% 256, (n %/% 256) %% 256, 0, 0)),
    as.raw(rep(0x5a, n)))
}
