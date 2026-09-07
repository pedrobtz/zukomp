# The R face of the C test harness. Not exported, documented as internal, and
# present in every build: chunk-boundary correctness is what zuhttp depends
# on, so it is testable from the stage that introduces the driver rather than
# from Stage 16 when an R streaming API finally exists.

#' Drive the C stream driver at chosen chunk sizes
#'
#' @param bytes Raw vector to feed.
#' @param codec Codec name.
#' @param mode `"encode"` or `"decode"`.
#' @param in_chunk,out_chunk Bytes of input offered, and output space made
#'   available, per `process()` call. Small values exercise the awkward
#'   boundaries; 1 is the harshest.
#' @param max_output,max_ratio Decoder limits; 0 means unlimited.
#' @param flush_every Issue `ZU_FLUSH` every n-th call, or `NULL` for never.
#' @param level Codec-native compression level, or `NULL` for the default.
#' @param reject_trailing Error on bytes left over after the stream ends.
#'   Defaults to `TRUE`, matching whole-buffer semantics.
#' @param concat_members Continue into a following gzip member. Defaults to
#'   `TRUE`, since RFC 1952 permits them and standard tools produce them.
#' @return A raw vector.
#' @keywords internal
#' @noRd
zu_test_stream <- function(bytes, codec, mode = c("encode", "decode"),
                           in_chunk = 4096, out_chunk = 4096,
                           max_output = 0, max_ratio = 0,
                           flush_every = NULL, level = NULL,
                           reject_trailing = TRUE, concat_members = TRUE) {
  mode <- match.arg(mode)
  stopifnot(is.raw(bytes), is.character(codec), length(codec) == 1L)

  res <- .Call(
    zukomp_test_stream,
    bytes, codec, identical(mode, "encode"),
    as.double(in_chunk), as.double(out_chunk),
    as.double(max_output), as.integer(max_ratio),
    as.double(flush_every %||% 0),
    if (is.null(level)) NULL else as.integer(level),
    isTRUE(reject_trailing), isTRUE(concat_members)
  )

  codes <- zu_status_codes()
  ok <- c(codes[["ZU_OK"]], codes[["ZU_STREAM_END"]])
  if (!res$status %in% ok) {
    zu_abort_status(res$status, codec = codec,
                    input_bytes = length(bytes),
                    output_bytes = length(res$bytes))
  }
  res$bytes
}

#' Provoke the growth arithmetic directly
#'
#' @param near_size_max Start from a buffer near `SIZE_MAX`.
#' @return Invisibly `TRUE`; raises a zukomp condition on failure.
#' @keywords internal
#' @noRd
zu_test_grow <- function(near_size_max = FALSE) {
  res <- .Call(zukomp_test_grow, near_size_max)
  if (res$status != zu_status_codes()[["ZU_OK"]]) {
    zu_abort_status(res$status)
  }
  invisible(TRUE)
}

`%||%` <- function(x, y) if (is.null(x)) y else x
