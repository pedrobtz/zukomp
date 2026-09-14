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
  # The chunk sizes and the limits are narrowed for C too, and a bare cast
  # of NA, Inf or a negative double to size_t is undefined behaviour. C has
  # a backstop, but validating here is what turns a wrong value into a
  # zukomp condition naming the argument.
  in_chunk <- zu_check_count(in_chunk, "in_chunk", codec = codec)
  out_chunk <- zu_check_count(out_chunk, "out_chunk", codec = codec)
  flush_every <- if (is.null(flush_every)) {
    0
  } else {
    zu_check_limit(flush_every, "flush_every", 2^53, codec = codec)
  }
  max_output <- zu_check_limit(max_output, "max_output", 2^53, codec = codec)
  max_ratio <- zu_check_limit(max_ratio, "max_ratio",
                              .Machine$integer.max, codec = codec)
  # Validate before narrowing, for the same reason komp_compress() does:
  # as.integer() on an out-of-range double yields NA with only a warning,
  # and NA_INTEGER is indistinguishable from ZU_LEVEL_DEFAULT in C.
  if (!is.null(level)) {
    if (!is.numeric(level) || length(level) != 1L || is.na(level) ||
        !is.finite(level) || level != trunc(level) ||
        level > .Machine$integer.max || level < -.Machine$integer.max) {
      zukomp_abort("zukomp_invalid_argument",
                   "`level` must be a single whole number, or NULL.",
                   codec = codec)
    }
  }

  res <- .Call(
    zukomp_test_stream,
    bytes, codec, identical(mode, "encode"),
    as.double(in_chunk), as.double(out_chunk),
    as.double(max_output), as.integer(max_ratio),
    as.double(flush_every),
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

#' Encode, reset with a new level, encode again
#'
#' Drives `zu_encoder_reset()`, which is public ABI and the shape a keep-alive
#' HTTP client uses, but which no R-level function reaches.
#'
#' @param bytes Raw vector to encode, twice.
#' @param codec Codec name.
#' @param level1,level2 Levels for the first and second stream, or `NULL`.
#' @return The raw bytes of the *second* stream.
#' @keywords internal
#' @noRd
zu_test_encoder_reset <- function(bytes, codec, level1 = NULL, level2 = NULL) {
  stopifnot(is.raw(bytes), is.character(codec), length(codec) == 1L)
  res <- .Call(
    zukomp_test_encoder_reset, bytes, codec,
    if (is.null(level1)) NULL else as.integer(level1),
    if (is.null(level2)) NULL else as.integer(level2)
  )
  codes <- zu_status_codes()
  if (!res$status %in% c(codes[["ZU_OK"]], codes[["ZU_STREAM_END"]])) {
    zu_abort_status(res$status, codec = codec)
  }
  res$bytes
}

#' Decompress through the one-shot C ABI at a fixed output capacity
#'
#' `komp_decompress()` grows its own sink, so `zu_decompress_one()` -- what a
#' consumer with a known Content-Length calls -- is otherwise unreachable
#' from R, and a zero-byte sink is unreachable at all.
#'
#' @param bytes Raw vector to decode.
#' @param codec Codec name.
#' @param cap Output capacity in bytes; 0 is legal and means a zero-byte sink.
#' @return A raw vector.
#' @keywords internal
#' @noRd
zu_test_decompress_one <- function(bytes, codec, cap) {
  stopifnot(is.raw(bytes), is.character(codec), length(codec) == 1L)
  if (!is.numeric(cap) || length(cap) != 1L || is.na(cap) || !is.finite(cap) ||
      cap < 0 || cap != trunc(cap)) {
    zukomp_abort("zukomp_invalid_argument",
                 "`cap` must be a single non-negative whole number.",
                 codec = codec)
  }
  res <- .Call(zukomp_test_decompress_one, bytes, codec, as.double(cap))
  codes <- zu_status_codes()
  if (!res$status %in% c(codes[["ZU_OK"]], codes[["ZU_STREAM_END"]])) {
    zu_abort_status(res$status, codec = codec, input_bytes = length(bytes))
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

#' Resolve the abstract level names against a synthetic vtable
#'
#' The `struct_size` forward-compatibility rules are unreachable from the
#' suite otherwise: registration is init-time only, from vtables this build
#' compiled itself, so a short vtable never occurs.
#'
#' @param case `0` advertised, `1` both left zero, `2` a vtable that predates
#'   the fields.
#' @return Integer `c(fast, best)`.
#' @keywords internal
#' @noRd
zu_test_vtable_levels <- function(case) {
  .Call(zukomp_test_vtable_levels, as.integer(case))
}

#' `zu_codec_get_info()` into a struct that predates the appended fields
#'
#' @param codec Codec name.
#' @param short Use the older, shorter `struct_size`?
#' @return Integer `c(status, level_min, level_default, level_fast, level_best)`.
#' @keywords internal
#' @noRd
zu_test_info_short <- function(codec, short) {
  .Call(zukomp_test_info_short, codec, isTRUE(short))
}

#' Compress through the one-shot C ABI into a bound-sized buffer
#'
#' `komp_compress()` grows its own sink, so `zu_compress_one()` and
#' `zu_compress_bound()` are otherwise reachable only from the consumer
#' package's `xor5a` -- a codec with no wrapper and `bound(n) == n`, which
#' is the one shape that cannot catch a bound that forgot a header.
#'
#' @param bytes Raw vector to compress.
#' @param codec Codec name.
#' @param level Codec-native level, or `NULL`.
#' @param cap_delta Capacity relative to the bound: `0` is exactly the
#'   bound, `-1` one byte short.
#' @return A raw vector, with the bound attached as the `bound` attribute.
#' @keywords internal
#' @noRd
zu_test_compress_one <- function(bytes, codec, level = NULL, cap_delta = 0) {
  stopifnot(is.raw(bytes), is.character(codec), length(codec) == 1L)
  res <- .Call(zukomp_test_compress_one, bytes, codec,
               if (is.null(level)) NULL else as.integer(level),
               as.double(cap_delta))
  bound <- attr(res, "bound")
  codes <- zu_status_codes()
  if (!res$status %in% c(codes[["ZU_OK"]], codes[["ZU_STREAM_END"]])) {
    zu_abort_status(res$status, codec = codec, input_bytes = length(bytes))
  }
  structure(res$bytes, bound = bound)
}

#' The bound alone, without compressing
#'
#' @inheritParams zu_test_compress_one
#' @return A single number.
#' @keywords internal
#' @noRd
zu_test_compress_bound <- function(bytes, codec, level = NULL) {
  attr(zu_test_compress_one(bytes, codec, level), "bound")
}

#' Decode two messages through one handle, resetting in between
#'
#' `zu_decoder_reset()` is public ABI with more state to get right than the
#' encoder's -- the limit budget, the wrapper state machine, the gzip header
#' parser and miniz's own stream -- and had no caller outside its own
#' definition.
#'
#' @param a,b The two compressed messages.
#' @param codec Codec name.
#' @param max_output Per-message output cap; 0 means unlimited.
#' @return The two decoded messages, concatenated.
#' @keywords internal
#' @noRd
zu_test_decoder_reset <- function(a, b, codec, max_output = 0) {
  stopifnot(is.raw(a), is.raw(b), is.character(codec), length(codec) == 1L)
  max_output <- zu_check_limit(max_output, "max_output", 2^53, codec = codec)
  res <- .Call(zukomp_test_decoder_reset, a, b, codec, as.double(max_output))
  codes <- zu_status_codes()
  if (!res$status %in% c(codes[["ZU_OK"]], codes[["ZU_STREAM_END"]])) {
    zu_abort_status(res$status, codec = codec,
                    input_bytes = length(a) + length(b),
                    output_bytes = length(res$bytes))
  }
  res$bytes
}

#' Reset a decoder onto a different codec, which must be refused
#'
#' @param from,to Codec names.
#' @return Invisibly `TRUE`; raises a zukomp condition with the refusal.
#' @keywords internal
#' @noRd
zu_test_decoder_reset_codec <- function(from, to) {
  res <- .Call(zukomp_test_decoder_reset_codec, from, to)
  codes <- zu_status_codes()
  if (!res$status %in% c(codes[["ZU_OK"]], codes[["ZU_STREAM_END"]])) {
    zu_abort_status(res$status, codec = from)
  }
  invisible(TRUE)
}

#' Output sinks currently allocated by the drive loop
#'
#' The other leak tests watch R's `Vcells`, which saw the old `R_alloc` sink
#' and cannot see the malloc'd one that replaced it.
#'
#' @return A single number; zero when nothing is in flight.
#' @keywords internal
#' @noRd
zu_test_outbuf_live <- function() {
  .Call(zukomp_test_outbuf_live)
}
