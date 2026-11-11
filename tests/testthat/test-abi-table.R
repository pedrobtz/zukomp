# The registered C-callable table: how the ABI actually reaches another
# package, since LinkingTo supplies headers and not object code.

test_that("the API table is self-describing", {
  expect_identical(zu_abi_version(), 1L)
  expect_gt(zu_api_struct_size(), 0L)
})

test_that("the table reports the version and size it was built with", {
  api <- zu_get_api(1L)
  expect_false(is.null(api))
  expect_identical(unname(api[["abi_version"]]), 1L)
  expect_identical(unname(api[["struct_size"]]), zu_api_struct_size())
})

test_that("a future ABI request is refused, not guessed", {
  # Returning a best-effort table to a consumer that disagrees about its
  # layout would be a wild call through mismatched offsets. NULL is a clean
  # error at their call site instead.
  expect_null(zu_get_api(999L))
  expect_null(zu_get_api(2L))
  expect_null(zu_get_api(0L))
})

test_that("the R-visible ABI version matches the installed header", {
  # The header is what a consumer compiles against; the library is what
  # they call. If these disagree, every consumer is built against a lie.
  header <- installed_header_code()
  line <- grep("define ZUKOMP_ABI_VERSION", header, value = TRUE)
  expect_length(line, 1L)
  expect_identical(as.integer(sub("\\D+", "", line)), zu_abi_version())
})

test_that("zukomp-r.h is installed alongside zukomp.h", {
  # LinkingTo consumers get whatever is in inst/include; a resolver header
  # that failed to ship would strand them.
  path <- system.file("include", "zukomp-r.h", package = "zukomp")
  expect_true(nzchar(path) && file.exists(path))
})

test_that("the resolver header is lazy, not init-time", {
  # design 15: `Imports: zukomp` does not load zukomp's namespace unless
  # NAMESPACE has a real import directive, so resolving at DLL init can
  # fail. The fix is a cached first-use lookup, and this is the test that
  # the fix stays in.
  src <- installed_header_code("zukomp-r.h")
  expect_gt(length(grep("cached", src, fixed = TRUE)), 0L)
  expect_gt(length(grep("R_GetCCallable", src)), 0L)
})

test_that("the resolver header compiles under a consumer's strict flags", {
  # R_GetCCallable returns DL_FUNC, and casting that straight to the table's
  # signature fails -Wcast-function-type-mismatch -- in the *consumer's*
  # build, not ours. The union is what keeps this header usable by a package
  # with -Wall -Wextra -Werror.
  src <- installed_header_code("zukomp-r.h")
  expect_gt(length(grep("union", src, fixed = TRUE)), 0L)
})

test_that("the resolver is inert in a translation unit that does not use it", {
  # A header-defined plain `static` function warns as unused in every TU
  # that includes the header without calling it, which is a hard failure
  # for a consumer building with -Werror. `static inline` does not.
  src <- installed_header_code("zukomp-r.h")
  expect_gt(length(grep("static inline const zukomp_api_v1 \\*zukomp_api", src)), 0L)
})

test_that("the one-shot functions are declared in the public header", {
  header <- installed_header_code()
  for (fn in c("zu_compress_bound", "zu_compress_one", "zu_decompress_one")) {
    expect_gt(length(grep(fn, header, fixed = TRUE)), 0L, label = fn)
  }
})
