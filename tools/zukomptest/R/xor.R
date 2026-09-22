#' Is the externally registered codec present?
#' @export
xor5a_available <- function() {
  "xor5a" %in% komp_codecs()$id
}

#' Round-trip bytes through zukomp's one-shot C entry points
#'
#' Exercises the API table rather than the R API, which is the path a
#' package such as zuhttp actually takes.
#' @param x A raw vector.
#' @export
xor5a_roundtrip_via_c <- function(x) {
  stopifnot(is.raw(x))
  .Call(zukomptest_roundtrip_via_c, x)
}
