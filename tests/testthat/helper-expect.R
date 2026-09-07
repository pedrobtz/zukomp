# Domain expectations. The point is failure messages that say what broke in
# zukomp's terms rather than in testthat's.

# Asserts both the condition class -- which is the contract -- and that the
# condition carries the metadata design 7 requires. A condition with the
# right class but no `codec` field is still a bug: zuhttp branches on these
# fields, not on the message.
expect_codec_error <- function(expr, class) {
  err <- expect_error(expr, class = class)
  expect_true(
    all(c("codec", "input_bytes", "output_bytes", "native_status") %in% names(err)),
    info = paste0(
      "condition of class '", class, "' is missing design 7 metadata; has: ",
      paste(names(err), collapse = ", ")
    )
  )
  invisible(err)
}

# Round-trips through the C driver at the given chunk sizes, in both
# directions. Deliberately does not use komp_compress(): the whole point is
# to test the streaming path, and the whole-buffer API does not exist until
# Stage 9 anyway.
expect_chunked_roundtrip <- function(x, codec, in_chunk, out_chunk) {
  z <- zu_test_stream(x, codec = codec, mode = "encode",
                      in_chunk = in_chunk, out_chunk = out_chunk)
  got <- zu_test_stream(z, codec = codec, mode = "decode",
                        in_chunk = in_chunk, out_chunk = out_chunk)
  expect_identical(
    got, x,
    info = sprintf("codec=%s in_chunk=%d out_chunk=%d", codec, in_chunk, out_chunk)
  )
  invisible(z)
}

# Compress then decompress through the C driver and demand the bytes back
# exactly. Built on zu_test_stream() rather than komp_compress(), which does
# not exist until Stage 9.
expect_roundtrip <- function(x, codec, level = NULL) {
  z <- zu_test_stream(x, codec = codec, mode = "encode", level = level)
  expect_type(z, "raw")
  got <- zu_test_stream(z, codec = codec, mode = "decode")
  expect_identical(
    got, x,
    info = sprintf("codec=%s level=%s n=%d", codec,
                   if (is.null(level)) "default" else level, length(x))
  )
  invisible(z)
}

# Reads one committed fixture and the plaintext it should decode to.
fixture_bytes <- function(codec, file) {
  path <- test_path("fixtures", codec, file)
  readBin(path, "raw", file.size(path))
}

fixture_manifest <- function(codec = NULL) {
  m <- read.delim(test_path("fixtures", "MANIFEST.tsv"), stringsAsFactors = FALSE)
  if (!is.null(codec)) m <- m[m$codec %in% codec, , drop = FALSE]
  m
}

# A fixture's expected plaintext. `members` > 1 means concatenated members,
# whose payloads decode to the concatenation of the parts.
fixture_plaintext <- function(row) {
  one <- new_payload(row$payload, row$n)
  if (row$members > 1L) rep(one, row$members) else one
}
