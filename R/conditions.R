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
