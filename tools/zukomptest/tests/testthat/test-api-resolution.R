# zukomp-r.h's two ways of not getting a table, tested apart (#84).
#
# The header used to say resolution returns NULL when zukomp is unavailable.
# It does not: R_GetCCallable() raises before any NULL check runs. NULL means
# one thing, an ABI version zukomp does not serve.

test_that("the compiled-against ABI version gets a table", {
  expect_true(api_for_version(1L))
})

test_that("an ABI version zukomp does not serve gets NULL, not an error", {
  # Newer than the build, or 0. Every version up to the build's own is
  # served (decision 32); this build is version 1, so that is only 1.
  expect_false(api_for_version(2L))
  expect_false(api_for_version(0L))
})

test_that("a callable zukomp never registered raises instead of returning NULL", {
  # The branch a consumer reaches when zukomp's DLL has not registered
  # zukomp_get_api: an R error naming the function, never a NULL.
  expect_error(resolve_callable("zukomp_no_such_callable"),
               "zukomp_no_such_callable")
  expect_true(is.function(zukomp::komp_codecs))
})
