# zukomp's zstd and lz4 codecs through the C table, one shot each way: the
# shape a columnar-format consumer uses (zuarrow, qio). See src/oneshot.c.

#' One-shot compression through zukomp's C table
#' @param bytes Raw vector.
#' @param codec Codec name.
#' @param level Codec-native level, or `NULL` for the default.
#' @return `list(status, bytes)`, `status` a `zu_status` name.
#' @export
compress_one_via_c <- function(bytes, codec, level = NULL) {
  .Call(zukomptest_compress_one, bytes, codec,
        if (is.null(level)) NULL else as.integer(level))
}

#' One-shot decompression through zukomp's C table
#' @param bytes Raw vector.
#' @param codec Codec name.
#' @param cap Output buffer size.
#' @param max_output Decoder output limit; 0 for none.
#' @param expected_size The decoded size the container recorded, passed as
#'   `ZU_DEC_EXPECTED_SIZE`, or `NULL`.
#' @param old_struct Build the options as a consumer compiled against the
#'   original header would.
#' @return `list(status, bytes)`.
#' @export
decompress_one_via_c <- function(bytes, codec, cap, max_output = 0,
                                 expected_size = NULL, old_struct = FALSE) {
  .Call(zukomptest_decompress_one, bytes, codec, as.double(cap),
        as.double(max_output),
        if (is.null(expected_size)) NULL else as.double(expected_size),
        isTRUE(old_struct))
}

#' A codec's levels as `zu_codec_get_info()` reports them
#' @param codec Codec name.
#' @export
codec_levels_via_c <- function(codec) {
  .Call(zukomptest_codec_levels, codec)
}
