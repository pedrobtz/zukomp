#' Decompress a raw vector
#'
#' @param x A raw vector holding a complete compressed stream.
#' @param codec Codec name, as listed in [komp_codecs()].
#' @param max_output Refuse to produce more than this many bytes. Defaults
#'   to 1 GiB, overridable with `options(zukomp.max_output = )`. Use `0` for
#'   unlimited, deliberately. Compressed input from an untrusted source can
#'   expand enormously, and the cap is enforced by the core stream driver,
#'   so no codec can bypass it.
#' @param max_ratio Refuse to expand by more than this factor. `NULL` (the
#'   default) means no ratio limit: legitimately compressible data routinely
#'   exceeds any safe-looking threshold, so this is opt-in.
#' @return A raw vector.
#' @seealso [komp_compress()], [komp_codecs()]
#' @export
#' @examples
#' z <- komp_compress(charToRaw(strrep("data ", 200)), "gzip")
#' rawToChar(komp_decompress(z, "gzip"))
#'
#' # a small input that would expand a long way is stopped, not allocated
#' bomb <- komp_compress(raw(1e6), "gzip")
#' length(bomb)
#' try(komp_decompress(bomb, "gzip", max_output = 1024))
komp_decompress <- function(x,
                            codec = "auto",
                            max_output = getOption("zukomp.max_output", 1024^3),
                            max_ratio = getOption("zukomp.max_ratio", NULL)) {
  zu_check_raw(x)
  zu_check_codec_name(codec)

  if (identical(codec, "auto")) {
    codec <- zu_detect_or_abort(x)
  }

  if (!is.numeric(max_output) || length(max_output) != 1L || is.na(max_output) ||
      max_output < 0) {
    zukomp_abort("zukomp_invalid_argument",
                 "`max_output` must be a single non-negative number.",
                 codec = codec)
  }
  if (is.null(max_ratio)) {
    max_ratio <- 0L
  }
  if (!is.numeric(max_ratio) || length(max_ratio) != 1L || is.na(max_ratio) ||
      max_ratio < 0) {
    zukomp_abort("zukomp_invalid_argument",
                 "`max_ratio` must be a single non-negative number, or NULL.",
                 codec = codec)
  }

  res <- .Call(zukomp_decompress, x, codec,
               as.double(max_output), as.integer(max_ratio))
  zu_finish(res, codec, x)
}

# Resolves codec = "auto". Refusing to guess is the correct answer for a
# headerless format, so a failure here names the problem precisely rather
# than falling back to a codec that would return plausible-looking garbage.
zu_detect_or_abort <- function(x, call = sys.call(-1L)) {
  codec <- komp_detect(x)
  if (is.na(codec)) {
    zukomp_abort(
      "zukomp_undetectable_codec",
      paste0(
        "Could not identify a codec from these bytes. Headerless formats ",
        "such as \"deflate-raw\" cannot be detected and must be named ",
        "explicitly via `codec`."
      ),
      input_bytes = length(x),
      call = call
    )
  }
  codec
}
