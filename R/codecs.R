#' The codec capability table
#'
#' Every codec this build of zukomp knows the name of, whether or not an
#' implementation is present, plus any codec registered by another package.
#' This is the R-visible face of the registry: it is how you discover what
#' can be compressed, at what levels, and which HTTP content-coding each
#' codec corresponds to.
#'
#' A codec that is declared but not installed still gets a row, with
#' `available = FALSE` and `NA` for everything capability-shaped. That is
#' deliberate: "zstd exists but you need the zukomp.zstd package" is a more
#' useful answer than pretending the codec does not exist.
#'
#' @return A data frame with one row per codec and the columns:
#'   \describe{
#'     \item{id}{Codec name, as accepted by the `codec` argument elsewhere.}
#'     \item{available}{Is an implementation registered?}
#'     \item{can_encode, can_decode}{Supported directions; `NA` if unavailable.}
#'     \item{level_min, level_max, level_default}{Codec-native compression
#'       levels. `NA` when the codec has no level axis, and when it is
#'       unavailable. Levels are not comparable between codecs.}
#'     \item{detectable}{Can `komp_detect()` (Stage 10) recognise this codec from its
#'       bytes? Headerless formats cannot be detected and must be named.}
#'     \item{content_encoding}{The HTTP content-coding token, or `NA`.}
#'     \item{source}{Package that registered the implementation, or `NA`.}
#'   }
#' @export
#' @examples
#' codecs <- komp_codecs()
#' codecs[, c("id", "available", "content_encoding")]
#'
#' # what this build can actually decompress right now
#' codecs$id[which(codecs$can_decode)]
komp_codecs <- function() {
  cols <- .Call(zukomp_codec_table)
  names(cols) <- c(
    "id", "available", "can_encode", "can_decode",
    "level_min", "level_max", "level_default",
    "detectable", "content_encoding", "source"
  )
  as.data.frame(cols, stringsAsFactors = FALSE)
}

#' Is a codec implementation available?
#'
#' @param codec A codec name, as listed in [komp_codecs()]'s `id` column.
#' @return `TRUE` if an implementation is registered, `FALSE` if the codec is
#'   known to zukomp but not installed. An unknown name is an error of class
#'   `zukomp_unsupported_codec`, since it is far more likely to be a typo
#'   than a deliberate probe.
#' @export
#' @examples
#' komp_codec_available("identity")
#'
#' # a codec zukomp knows of, but which ships in a separate package
#' komp_codec_available("zstd")
komp_codec_available <- function(codec) {
  if (!is.character(codec) || length(codec) != 1L || is.na(codec)) {
    zukomp_abort(
      "zukomp_invalid_argument",
      "`codec` must be a single, non-missing codec name."
    )
  }
  available <- .Call(zukomp_codec_available, codec)
  if (is.na(available)) {
    abort_unsupported_codec(codec)
  }
  available
}
