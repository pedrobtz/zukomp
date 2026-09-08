#' Build and provenance information
#'
#' What this build of zukomp actually contains: its version, the ABI
#' version other packages link against, the codecs it registered, and the
#' vendored sources compiled into it with the trim applied to them.
#'
#' Reported from the compiled library rather than read from
#' `tools/vendor/manifest.tsv`, because the manifest is not installed and
#' what matters here is what was actually built.
#'
#' @return A list with `version`, `abi_version`, `codecs`, `vendored` and
#'   `build_flags`.
#' @export
#' @examples
#' info <- komp_info()
#' info$version
#' info$vendored
#' info$build_flags
komp_info <- function() {
  codecs <- komp_codecs()
  list(
    version = utils::packageVersion("zukomp"),
    abi_version = zu_abi_version(),
    codecs = codecs$id[codecs$available],
    vendored = zu_vendored(),
    build_flags = .Call(zukomp_build_info)
  )
}

# Vendored sources and the versions actually compiled in, as a data frame.
# Reported from the library, never from tools/vendor/manifest.tsv, which is
# not installed.
zu_vendored <- function() {
  v <- .Call(zukomp_vendored)
  data.frame(
    source = names(v),
    version = unname(v),
    stringsAsFactors = FALSE
  )
}
