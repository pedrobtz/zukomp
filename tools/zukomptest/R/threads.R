# The thread contract of zukomp_api_v1 (#133): see src/threads.c and
# src/zkt_threads.h.

#' Decode and encode zstd and lz4 on several threads through zukomp's table
#'
#' Every thread has its own decoders and encoders and compares each output
#' with the single-threaded one.
#' @param threads Number of threads.
#' @param rounds Passes over every codec and size, per thread.
#' @return The number of comparisons made; an error names the first that
#'   failed.
#' @export
decode_on_threads <- function(threads = 2L, rounds = 1L) {
  .Call(zukomptest_threads, as.integer(threads), as.integer(rounds))
}

#' One Parquet-style page at sink sizes n - 1, n and n + 1
#' @return The number of cases checked; an error names the first that failed.
#' @export
page_caps_via_c <- function() {
  .Call(zukomptest_page_caps)
}
