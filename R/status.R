#' Description of every ABI status code
#'
#' Walks the `zu_status` enum from `ZU_OK` to `ZU_ERR_INTERNAL` and returns
#' `zu_status_string()` for each. Internal: the R condition hierarchy, not
#' these strings, is the contract for callers.
#'
#' @return A character vector, one entry per status, in enum order.
#' @keywords internal
#' @noRd
zu_all_status_strings <- function() {
  .Call(zukomp_all_status_strings)
}

#' ABI version implemented by this build
#'
#' @return A length-1 integer.
#' @keywords internal
#' @noRd
zu_abi_version <- function() {
  .Call(zukomp_abi_version)
}
