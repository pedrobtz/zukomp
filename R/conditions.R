# The R side of design 7's error model. C returns status codes; R constructs
# conditions. Messages stay one line and are allowed to be reworded; the
# *class* is the contract callers branch on, and the data is what makes
# programmatic handling possible.
#
# Stage 9 extends this to the full hierarchy as the failure modes come
# online. The shape of a condition is fixed here so it does not have to be
# retrofitted: every zukomp error carries codec, input_bytes, output_bytes
# and native_status, even when a given failure knows only some of them.

zukomp_abort <- function(class,
                         message,
                         codec = NA_character_,
                         input_bytes = NA_real_,
                         output_bytes = NA_real_,
                         native_status = NA_integer_,
                         call = sys.call(-1L)) {
  cond <- structure(
    class = c(class, "zukomp_error", "error", "condition"),
    list(
      message = message,
      call = call,
      codec = codec,
      input_bytes = input_bytes,
      output_bytes = output_bytes,
      native_status = native_status
    )
  )
  stop(cond)
}

# Raised for a codec name this build has never heard of. Distinct from a
# known codec whose implementation is absent, which is not an error at all:
# komp_codec_available() reports FALSE and komp_codecs() shows the row with
# available = FALSE.
abort_unsupported_codec <- function(codec, call = sys.call(-1L)) {
  known <- komp_codecs()$id
  zukomp_abort(
    "zukomp_unsupported_codec",
    sprintf(
      "Unknown codec %s. Known codecs: %s.",
      encodeString(codec, quote = '"'),
      paste(known, collapse = ", ")
    ),
    codec = codec,
    call = call
  )
}

# zu_status enum values, by C enumerator name. Fetched from C rather than
# hardcoded so that renumbering the enum cannot silently remap conditions.
zu_status_codes <- local({
  cache <- NULL
  function() {
    if (is.null(cache)) cache <<- .Call(zukomp_status_codes)
    cache
  }
})

# design 7's mapping from a native status to a condition class. Statuses that
# are not failures have no class and must never reach zukomp_abort().
zu_status_class <- c(
  ZU_ERR_INVALID_ARGUMENT = "zukomp_invalid_argument",
  ZU_ERR_UNSUPPORTED      = "zukomp_unsupported_codec",
  ZU_ERR_INVALID_DATA     = "zukomp_invalid_data",
  ZU_ERR_TRUNCATED        = "zukomp_truncated",
  ZU_ERR_CHECKSUM         = "zukomp_checksum_error",
  ZU_ERR_TRAILING         = "zukomp_trailing_bytes",
  ZU_ERR_MEMORY           = "zukomp_memory_error",
  ZU_ERR_OUTPUT_LIMIT     = "zukomp_output_limit",
  ZU_ERR_RATIO_LIMIT      = "zukomp_ratio_limit",
  ZU_ERR_INTERNAL         = "zukomp_internal_error"
)

# Raises the condition matching a native status. `status` is an integer from
# the C layer; anything that is not a failure is a programming error here,
# not something to paper over with a generic message.
zu_abort_status <- function(status,
                            codec = NA_character_,
                            input_bytes = NA_real_,
                            output_bytes = NA_real_,
                            call = sys.call(-1L)) {
  codes <- zu_status_codes()
  name <- names(codes)[match(status, codes)]
  if (is.na(name) || !name %in% names(zu_status_class)) {
    zukomp_abort(
      "zukomp_internal_error",
      sprintf("Unexpected native status %d.", status),
      codec = codec, native_status = status, call = call
    )
  }
  zukomp_abort(
    unname(zu_status_class[[name]]),
    zu_status_message(name, codec),
    codec = codec,
    input_bytes = input_bytes,
    output_bytes = output_bytes,
    native_status = status,
    call = call
  )
}

# One line, and phrased for the person who hit it rather than for the enum.
zu_status_message <- function(name, codec) {
  what <- if (is.na(codec)) "stream" else paste0(codec, " stream")
  switch(
    name,
    ZU_ERR_INVALID_ARGUMENT = "Invalid argument.",
    ZU_ERR_UNSUPPORTED      = sprintf("Codec %s is not available.",
                                      encodeString(codec, quote = '"')),
    ZU_ERR_INVALID_DATA     = sprintf("Invalid compressed data in %s.", what),
    ZU_ERR_TRUNCATED        = sprintf("Truncated %s.", what),
    ZU_ERR_CHECKSUM         = sprintf("Checksum mismatch in %s.", what),
    ZU_ERR_TRAILING         = sprintf("Unexpected trailing bytes after %s.", what),
    ZU_ERR_MEMORY           = "Out of memory, or a size computation overflowed.",
    ZU_ERR_OUTPUT_LIMIT     = "Decompressed output exceeded `max_output`.",
    ZU_ERR_RATIO_LIMIT      = "Compression ratio exceeded `max_ratio`.",
    ZU_ERR_INTERNAL         = "Internal error in zukomp."
  )
}
