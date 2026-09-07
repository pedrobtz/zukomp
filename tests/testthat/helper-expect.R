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
