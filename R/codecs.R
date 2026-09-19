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
#'     \item{level_fast, level_best}{Where the abstract levels `"fast"` and
#'       `"best"` land for this codec. These are *not* `level_min` and
#'       `level_max`: for the DEFLATE family level 0 is stored blocks, so
#'       `"fast"` is 1, and a codec whose level is an acceleration factor
#'       inverts the mapping entirely. Only the codec knows, so it declares
#'       them.}
#'     \item{detectable}{Can `komp_detect()` recognise this codec from its
#'       bytes? Headerless formats cannot be detected and must be named.}
#'     \item{can_flush}{Does the codec support a mid-stream flush -- "put the
#'       bytes on the wire now"? `NA` if unavailable. A caller streaming a
#'       request body should check this before committing to a codec, rather
#'       than discovering it mid-body.}
#'     \item{content_encoding}{The HTTP content-coding token, or `NA`.}
#'     \item{source}{Package that registered the implementation, or `NA`.}
#'   }
#' @references
#' The built-in codecs implement the formats specified in
#' Deutsch, P. (1996) "DEFLATE Compressed Data Format Specification version
#' 1.3", RFC 1951, \doi{10.17487/RFC1951};
#' Deutsch, P. and Gailly, J-L. (1996) "ZLIB Compressed Data Format
#' Specification version 3.3", RFC 1950, \doi{10.17487/RFC1950}; and
#' Deutsch, P. (1996) "GZIP file format specification version 4.3",
#' RFC 1952, \doi{10.17487/RFC1952}.
#' @export
#' @examples
#' codecs <- komp_codecs()
#' codecs[, c("id", "available", "content_encoding")]
#'
#' # what this build can actually decompress right now
#' codecs$id[which(codecs$can_decode)]
komp_codecs <- function() {
  zu_codec_table()
}

# The frame itself, memoised.
#
# zukomp's own registry is written once, from R_init_zukomp, but a satellite
# package's DLL can load at any point afterwards, so the cache has to be
# guarded rather than assumed final. Worth caching because every
# komp_compress() and komp_decompress() call validates its arguments against
# this table, once or twice, and rebuilding a dozen parallel vectors into a
# data frame dominates the compression of a small HTTP body.
#
# The key is the registry's mutation counter, not the number of rows the
# table displays. Row count is not a function of registry state: a satellite
# implementing a codec zukomp already *declares* -- zstd, say -- flips that
# row from available = FALSE to TRUE without adding one. Keyed on rows, a
# table warmed before the satellite loaded stayed stale, so komp_compress(
# codec = "zstd") rejected the codec as not installed while
# komp_codec_available("zstd") returned TRUE, because that asks the registry
# directly. Which behaviour you got depended on DLL load order.
zu_codec_table <- local({
  cache <- NULL
  generation <- NULL
  function() {
    n <- .Call(zukomp_registry_generation)
    if (is.null(cache) || !identical(n, generation)) {
      cols <- .Call(zukomp_codec_table)
      names(cols) <- c(
        "id", "available", "can_encode", "can_decode",
        "level_min", "level_max", "level_default",
        "level_fast", "level_best",
        "detectable", "can_flush", "content_encoding", "source"
      )
      cache <<- as.data.frame(cols, stringsAsFactors = FALSE)
      generation <<- n
    }
    cache
  }
})

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
