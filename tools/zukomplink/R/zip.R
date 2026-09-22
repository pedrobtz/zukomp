# The R face of the archive consumer. Nothing here touches zukomp: see
# NAMESPACE for why that is the point rather than an omission.

# The C layer raises with Rf_error(), which produces a plain simpleError.
# zukomp's own testing convention is to assert on condition classes and never
# on message text, so that a rewording is not forty broken tests -- a fixture
# has no licence to be looser about that than the package it is testing, and
# these messages are the ones a zuxlsx developer will read when the archive
# wiring is wrong. Re-raising is the cheapest way to get a class onto them
# without teaching the C layer about condition objects.
zl_classed <- function(expr) {
  withCallingHandlers(
    expr,
    error = function(e) {
      stop(errorCondition(conditionMessage(e),
                          class = c("zukomplink_error", "zukomplink_condition"),
                          call = sys.call(-1)))
    }
  )
}

zip_members <- function(path) {
  zl_classed(.Call(C_zip_members, path.expand(path)))
}

# chunk = 0 reads each member in one call; any positive value is taken
# literally, down to one byte per call. A pull-style reader is only a
# streaming reader if its output does not depend on that number.
zip_extract <- function(path, name, chunk = 0L) {
  zl_classed(.Call(C_zip_extract, path.expand(path), name, as.integer(chunk)))
}

miniz_version <- function() {
  .Call(C_miniz_version)
}
