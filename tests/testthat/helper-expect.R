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
