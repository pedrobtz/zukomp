test_that("the package's shared object is loaded and registered", {
  dll <- getLoadedDLLs()[["zukomp"]]
  expect_s3_class(dll, "DLLInfo")
})

test_that("dynamic symbol lookup is disabled", {
  # R_useDynamicSymbols(dll, FALSE) means an unregistered symbol must not
  # resolve. Guards against a later stage dropping the call from init.c.
  expect_error(getNativeSymbolInfo("R_init_zukomp", PACKAGE = "zukomp"))
})
