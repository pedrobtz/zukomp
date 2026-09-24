#' Compress a raw vector
#'
#' Bytes in, bytes out. Character input is not accepted: encoding is a
#' decision the caller must make explicitly, so convert with
#' [charToRaw()] or `iconv()` first.
#'
#' @param x A raw vector.
#' @param codec Codec name, as listed in [komp_codecs()]. Defaults to
#'   `"gzip"`, which interoperates with everything.
#' @param level Compression level. Either a codec-native whole number, one of
#'   the abstract names `"fast"`, `"default"` and `"best"`, or `NULL` for the
#'   codec's own default.
#'
#'   Numeric levels are **not** comparable between codecs: `6` means different
#'   things to gzip and to zstd, and [komp_codecs()] publishes each codec's
#'   valid range. The abstract names are therefore the portable way to say
#'   "compress harder" -- they resolve per codec against that range, and work
#'   on every codec, including ones with no level axis at all.
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
#'
#' # abstract levels port across codecs; numeric ones do not
#' length(komp_compress(x, level = "fast"))
#' length(komp_compress(x, level = "best"))
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

zu_check_codec_name <- function(codec, call = sys.call(-1L), allow_auto = FALSE) {
  if (!is.character(codec) || length(codec) != 1L || is.na(codec)) {
    zukomp_abort(
      "zukomp_invalid_argument",
      "`codec` must be a single, non-missing codec name.",
      call = call
    )
  }
  if (identical(codec, "auto")) {
    # "auto" is a decompression instruction, not a codec. Letting it reach
    # the registry produces "not available", which reads as "install a
    # satellite package" for something that is not a compression option
    # at all.
    if (allow_auto) {
      return(invisible(codec))
    }
    zukomp_abort(
      "zukomp_invalid_argument",
      "`codec = \"auto\"` selects a codec by detection and only makes sense when decompressing. Name a codec to compress with.",
      call = call
    )
  }
  d <- komp_codecs()
  row <- d[d$id == codec, ]
  if (nrow(row) == 0L) {
    # A name this build has never heard of: far more likely a typo than a
    # deliberate probe, so the message lists what is on offer.
    abort_unsupported_codec(codec, call = call)
  }
  if (!row$available) {
    # Known, but with no implementation here. The name is reserved so a
    # future satellite can claim it with its declared id; until one exists
    # there is no package to point at, so the message must not suggest
    # installing one (#33).
    zukomp_abort(
      "zukomp_unsupported_codec",
      sprintf(
        "Codec \"%s\" is reserved by zukomp but not installed in this build: no implementation of it has been released yet.",
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
  if (is.character(level)) {
    return(zu_level_from_name(level, codec, call = sys.call(-1L)))
  }
  # Order matters here. `level != as.integer(level)` was the original test,
  # and as.integer() returns NA for anything outside integer range, so the
  # comparison was NA and the `if` failed with a bare R error instead of a
  # zukomp condition. Check representability first, narrow afterwards.
  if (!is.numeric(level) || length(level) != 1L || is.na(level) ||
      !is.finite(level) || level != trunc(level) ||
      level > .Machine$integer.max || level < -.Machine$integer.max) {
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

# The abstract level names of design 4, resolved against the codec's own
# advertised range.
#
# These are the *only* cross-codec way to say "compress harder": numeric
# levels are codec-native and deliberately not comparable, so a caller
# writing codec-agnostic code has no other correct option. Resolution happens
# here rather than in C: the C ABI's level is an int32_t plus
# ZU_LEVEL_DEFAULT and stays that way, so a satellite codec gets the names
# for free just by advertising [level_min, level_max].
zu_level_names <- c("fast", "default", "best")

zu_level_from_name <- function(level, codec, call = sys.call(-1L)) {
  if (length(level) != 1L || is.na(level) || !level %in% zu_level_names) {
    zukomp_abort(
      "zukomp_invalid_argument",
      sprintf(
        "`level` must be one of %s, a codec-native whole number, or NULL.",
        paste0("\"", zu_level_names, "\"", collapse = ", ")
      ),
      codec = codec, call = call
    )
  }
  row <- komp_codecs()
  row <- row[row$id == codec, ]
  # A codec with no level axis has exactly one behaviour, so all three names
  # denote it. Rejecting "fast" here would mean codec-agnostic code still has
  # to special-case the level axis, which is what the names exist to avoid
  # (design 4, as amended).
  if (nrow(row) != 1L || is.na(row$level_min)) {
    return(NULL)
  }
  switch(level,
    # NULL, not row$level_default: ZU_LEVEL_DEFAULT asks the codec itself,
    # which stays right even if this table were ever stale.
    default = NULL,
    # level_fast/level_best, never level_min/level_max. DEFLATE's level 0 is
    # stored blocks, so "fast" resolved to the range's floor would expand the
    # input; a codec whose level is an acceleration factor inverts the
    # mapping outright. The codec declares both, the core never derives them.
    fast = row$level_fast,
    best = row$level_best
  )
}

# Validates a limit before it is narrowed for C.
#
# R numerics are doubles; the C side takes a uint64_t (max_output) or a
# uint32_t (max_ratio). Narrowing without checking is how a requested limit
# becomes something else entirely: as.integer() yields NA for anything past
# .Machine$integer.max, which C then reads as 2147483648, and casting a
# non-finite double to an integer type is undefined behaviour -- on exactly
# the sanitizer builds this package's CI exists to keep clean.
#
# Inf is accepted and means "no limit", which is what 0 means to the C
# layer. Spelling it either way is deliberate; silently mangling a finite
# number the caller asked for is not.
zu_check_limit <- function(x, arg, upper, codec = NA_character_,
                           call = sys.call(-1L)) {
  if (is.null(x)) {
    return(0)
  }
  # A fractional limit must be refused, not rounded. Both of these are
  # narrowed to an integer type on the way to C -- max_output is cast to
  # uint64_t, max_ratio through as.integer() -- and C truncates toward zero.
  # Native 0 means "no limit", so *any* limit in (0, 1) truncated to 0 and
  # silently disabled the guard it was asked to impose: max_output = 0.5
  # decompressed a payload of any size at all. These two arguments are the
  # decompression-bomb guards, and they routinely arrive from options or
  # deserialised config rather than from integer literals, so a computed
  # fraction turning a restrictive policy into no policy is a real failure
  # mode. Neither floor nor ceiling can be assumed to be the caller's
  # intent, so the value is rejected.
  if (!is.numeric(x) || length(x) != 1L || is.na(x) || x < 0 ||
      (is.finite(x) && x != trunc(x))) {
    zukomp_abort(
      "zukomp_invalid_argument",
      sprintf(
        "`%s` must be a single non-negative whole number, NULL, or Inf for no limit.",
        arg
      ),
      codec = codec, call = call
    )
  }
  if (is.infinite(x)) {
    return(0)
  }
  if (x > upper) {
    zukomp_abort(
      "zukomp_invalid_argument",
      sprintf("`%s` must be at most %s, or Inf for no limit.",
              arg, format(upper, scientific = FALSE)),
      codec = codec, call = call
    )
  }
  x
}

# Validates a count of bytes that must be strictly positive -- a chunk size,
# where 0 would stall the driver and NA, Inf or a negative value would be
# undefined behaviour once cast to size_t.
zu_check_count <- function(x, arg, codec = NA_character_,
                           call = sys.call(-1L)) {
  if (!is.numeric(x) || length(x) != 1L || is.na(x) || !is.finite(x) ||
      x < 1 || x != trunc(x) || x > 2^53) {
    zukomp_abort(
      "zukomp_invalid_argument",
      sprintf("`%s` must be a single whole number of bytes, at least 1.", arg),
      codec = codec, call = call
    )
  }
  x
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
