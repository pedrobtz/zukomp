#' Version of the vendored miniz sources
#'
#' Stage 1 scaffolding. Reports the `MZ_VERSION` string of the miniz release
#' compiled into this package, which is pinned by
#' `tools/vendor/manifest.tsv`. Superseded by `komp_info()` in Stage 9.
#'
#' @return A length-1 character vector, e.g. `"11.3.2"`.
#' @keywords internal
#' @noRd
zu_miniz_version <- function() {
  .Call(zukomp_miniz_version)
}
