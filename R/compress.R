#' Compress a raw vector
#'
#' Bytes in, bytes out. Character input is not accepted: encoding is a
#' decision the caller must make explicitly, so convert with
#' [charToRaw()] or `iconv()` first.
#'
#' @param x A raw vector.
#' @param codec Codec name, as listed in [komp_codecs()]. Defaults to
#'   `"gzip"`, which interoperates with everything.
#' @param level Codec-native compression level, or `NULL` for the codec's
#'   own default. Levels are **not** comparable between codecs: `6` means
#'   different things to gzip and to zstd. [komp_codecs()] publishes each
#'   codec's valid range.
#' @return A raw vector.
#' @seealso [komp_decompress()], [komp_codecs()]
#' @export
#' @examples
#' x <- charToRaw(strrep("compress me ", 100))
#' z <- komp_compress(x)
#' length(x)
#' length(z)
#' identical(komp_decompress(z, "gzip"), x)
#'
#' # gzip output is deterministic: no timestamp, no filename
#' identical(komp_compress(x), komp_compress(x))
komp_compress <- function(x, codec = "gzip", level = NULL) {
  zu_check_raw(x)
  zu_check_codec_name(codec)
  level <- zu_check_level(level, codec)

  res <- .Call(zukomp_compress, x, codec, level)
  zu_finish(res, codec, x)
}

# -- shared argument checking -------------------------------------------------
# These raise before any C code runs, so a bad argument never reaches a
# state machine that would report it as a data error.

zu_check_raw <- function(x, arg = "x") {
  if (!is.raw(x)) {
    zukomp_abort(
      "zukomp_invalid_argument",
      sprintf(
        "`%s` must be a raw vector, not %s. zukomp is bytes in, bytes out; convert text with charToRaw() so the encoding is your decision.",
        arg, class(x)[[1L]]
      ),
      call = sys.call(-1L)
    )
  }
  invisible(x)
}

zu_check_codec_name <- function(codec, call = sys.call(-1L)) {
  if (!is.character(codec) || length(codec) != 1L || is.na(codec)) {
    zukomp_abort(
      "zukomp_invalid_argument",
      "`codec` must be a single, non-missing codec name.",
      call = call
    )
  }
  if (identical(codec, "auto")) {
    return(invisible(codec))
  }
  d <- komp_codecs()
  row <- d[d$id == codec, ]
  if (nrow(row) == 0L) {
    # A name this build has never heard of: far more likely a typo than a
    # deliberate probe, so the message lists what is on offer.
    abort_unsupported_codec(codec, call = call)
  }
  if (!row$available) {
    # Known, but its implementation ships elsewhere. Saying which package
    # to install is the whole reason declared names are tracked separately
    # from registered ones.
    zukomp_abort(
      "zukomp_unsupported_codec",
      sprintf(
        "Codec \"%s\" is known to zukomp but not installed in this build. It ships in a separate package.",
        codec
      ),
      codec = codec, call = call
    )
  }
  invisible(codec)
}

# Levels are validated against the codec's advertised range here as well as
# in the C driver. The duplication is deliberate: R can name the codec and
# its range in the message, which a status code cannot.
zu_check_level <- function(level, codec) {
  if (is.null(level)) {
    return(NULL)
  }
  if (length(level) != 1L || is.na(level) ||
      (!is.numeric(level)) || level != as.integer(level)) {
    zukomp_abort(
      "zukomp_invalid_argument",
      "`level` must be a single whole number, or NULL for the codec's default.",
      codec = codec, call = sys.call(-1L)
    )
  }
  level <- as.integer(level)
  row <- komp_codecs()
  row <- row[row$id == codec, ]
  if (nrow(row) == 1L && !is.na(row$level_min)) {
    if (level < row$level_min || level > row$level_max) {
      zukomp_abort(
        "zukomp_invalid_argument",
        sprintf("`level` must be between %d and %d for codec \"%s\", not %d.",
                row$level_min, row$level_max, codec, level),
        codec = codec, call = sys.call(-1L)
      )
    }
  } else if (nrow(row) == 1L && is.na(row$level_min) && level != 0L) {
    zukomp_abort(
      "zukomp_invalid_argument",
      sprintf("Codec \"%s\" has no compression levels; use level = NULL.", codec),
      codec = codec, call = sys.call(-1L)
    )
  }
  level
}

# Turns the C layer's (status, bytes) pair into either a raw vector or a
# condition. Every zukomp error is raised from here or from an argument
# check, never from C.
zu_finish <- function(res, codec, input) {
  codes <- zu_status_codes()
  if (!res$status %in% c(codes[["ZU_OK"]], codes[["ZU_STREAM_END"]])) {
    zu_abort_status(res$status, codec = codec,
                    input_bytes = length(input),
                    output_bytes = length(res$bytes),
                    call = sys.call(-1L))
  }
  res$bytes
}
