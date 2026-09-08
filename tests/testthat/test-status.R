test_that("every status enumerator has a real description", {
  strings <- zu_all_status_strings()

  expect_type(strings, "character")
  expect_false(any(is.na(strings)))
  expect_false(any(strings == ""))
  # zu_status_string()'s out-of-range fallback must never be reached by a
  # value inside the enum: that would mean a new enumerator went undescribed.
  expect_false(any(strings == "unknown"))
  expect_false(any(strings == "unrecognised status"))
})

test_that("status descriptions are distinct", {
  # Two statuses sharing a description would make an error message
  # ambiguous about which condition class it came from.
  strings <- zu_all_status_strings()
  expect_identical(anyDuplicated(strings), 0L)
})

test_that("the status enum has not silently changed shape", {
  # ZU_OK is 0 and ZU_ERR_INTERNAL is last, so the walk covers the enum.
  # Update this count deliberately when adding a status, never reflexively.
  expect_length(zu_all_status_strings(), 14L)
  expect_identical(zu_all_status_strings()[[1L]], "ok")
})

test_that("the build reports the ABI version the header declares", {
  expect_identical(zu_abi_version(), 1L)
})
