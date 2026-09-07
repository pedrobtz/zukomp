# The zuhttp integration contract (design 16), as far as it can be taken
# without an HTTP client. These are the four things zuhttp is told to do,
# written here so that "zukomp supplies what zuhttp needs" is a test result
# rather than an assertion in a design document.

#' Accept-Encoding, derived from zukomp's registry
#'
#' Contract point 1. The header is built by asking zukomp what it can
#' decode, never by hardcoding a list. Install a satellite codec and its
#' token appears here with no change to this function.
#'
#' `identity` is dropped: it is always acceptable and advertising it is
#' noise. That is a client policy decision, which is why it lives here and
#' not in zukomp.
#' @export
default_accept_encoding <- function() {
  tokens <- .Call(zukomptest_decodable_tokens)
  tokens <- setdiff(tokens, "identity")
  paste(tokens, collapse = ", ")
}

#' Resolve a content-coding token to a codec name
#'
#' `deflate` resolves to `zlib`. Servers that actually send headerless
#' DEFLATE under that token are handled by [decode_body()], not here: the
#' quirk is client policy and does not belong in the codec package.
#' @param token An HTTP content-coding token.
#' @export
codec_for_token <- function(token) {
  .Call(zukomptest_codec_for_token, as.character(token))
}

#' Decode a response body for a `Content-Encoding` header
#'
#' Contract points 2, 3 and 4.
#'
#' Multiple codings are legal and are applied in the order listed, so they
#' are undone right to left. Limits are the client's to choose and zukomp's
#' to enforce, and they apply per stage -- a chain must not be able to
#' launder a bomb through an intermediate coding.
#'
#' @param body Raw vector.
#' @param content_encoding The header value, e.g. `"gzip"` or `"gzip, deflate"`.
#' @param max_decompressed_bytes,max_decompression_ratio Client limits,
#'   mapped straight onto `zu_decoder_opts`.
#' @param max_stages Refuse absurdly long coding chains. A cap on the number
#'   of stages is the client's concern, not the codec package's.
#' @export
decode_body <- function(body, content_encoding,
                        max_decompressed_bytes = 1024^3,
                        max_decompression_ratio = NULL,
                        max_stages = 4L) {
  stopifnot(is.raw(body))
  if (is.null(content_encoding) || !nzchar(trimws(content_encoding))) {
    return(body)
  }
  tokens <- trimws(strsplit(content_encoding, ",", fixed = TRUE)[[1]])
  tokens <- tokens[nzchar(tokens)]
  if (length(tokens) > max_stages) {
    stop(sprintf("Content-Encoding has %d codings, more than the %d allowed.",
                 length(tokens), max_stages))
  }

  for (token in rev(tokens)) {
    body <- decode_one(body, token,
                       max_decompressed_bytes, max_decompression_ratio)
  }
  body
}

decode_one <- function(body, token, max_bytes, max_ratio) {
  if (identical(tolower(token), "identity")) {
    return(body)
  }
  codec <- codec_for_token(token)
  if (is.na(codec)) {
    stop(sprintf("Unsupported Content-Encoding %s.",
                 encodeString(token, quote = '"')))
  }

  out <- try_decode(body, codec, max_bytes, max_ratio)
  if (!inherits(out, "zukomp_error")) {
    return(out)
  }

  # Contract point 3: `Content-Encoding: deflate` is ambiguous in the wild.
  # Some servers send headerless DEFLATE under it. zukomp resolves the token
  # to zlib and stops there, deliberately; retrying as raw is this client's
  # policy, and it is attempted only when the zlib attempt failed on the
  # data itself -- not when it hit a limit, which would turn a bomb defence
  # into a second chance at the bomb.
  if (identical(tolower(token), "deflate") &&
      inherits(out, "zukomp_invalid_data")) {
    retry <- try_decode(body, "deflate-raw", max_bytes, max_ratio)
    if (!inherits(retry, "zukomp_error")) {
      return(retry)
    }
  }
  stop(out)
}

try_decode <- function(body, codec, max_bytes, max_ratio) {
  tryCatch(
    zukomp::komp_decompress(body, codec,
                            max_output = max_bytes,
                            max_ratio = max_ratio),
    zukomp_error = function(e) e
  )
}

#' Decode a body incrementally, never holding it whole
#'
#' The shape of design 24 criterion 11: the body is fed in chunks and
#' decoded into a single reused sink, so peak allocation is one chunk
#' regardless of the decoded size. Returns the length and a checksum rather
#' than the bytes, because materialising them would defeat the point.
#'
#' @param body Raw vector.
#' @param codec Codec name.
#' @param chunk Sink size in bytes.
#' @param max_output,max_ratio Limits, enforced by zukomp's driver.
#' @export
decode_incremental <- function(body, codec, chunk = 4096L,
                               max_output = 0, max_ratio = 0) {
  .Call(zukomptest_decode_incremental, body, as.character(codec),
        as.integer(chunk), as.double(max_output), as.integer(max_ratio))
}
