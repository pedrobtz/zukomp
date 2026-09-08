# Catches a test that mutates global state and forgets to clean up. The
# registry makes this worth doing from the first stage that has one: a test
# that registers a codec, or flips a zukomp option, would otherwise change
# the results of whatever runs next -- and with shuffled test order, "next"
# is a different file every run.
testthat::set_state_inspector(function() {
  opts <- options()
  # testthat sets diffobj.* lazily, the first time it renders a failure
  # diff, so they appear to leak out of whichever test happened to fail
  # first. Not ours, and not worth reporting.
  opts <- opts[!grepl("^diffobj\\.", names(opts))]
  list(
    options = opts,
    codecs = if (exists("komp_codecs")) komp_codecs()$id else NULL
  )
})
