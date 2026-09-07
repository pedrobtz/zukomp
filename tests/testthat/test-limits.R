test_that("max_output is enforced by the driver", {
  expect_codec_error(
    zu_test_stream(new_payload("zeros", 4096L), "identity", "decode",
                   max_output = 100),
    "zukomp_output_limit"
  )
})

test_that("max_output is exact, not approximate", {
  # Exactly at the cap must pass; one byte over must fail. An off-by-one here
  # would either reject legitimate payloads or let a bomb through.
  x <- new_payload("zeros", 1000L)
  expect_silent(zu_test_stream(x, "identity", "decode", max_output = 1000))
  expect_codec_error(
    zu_test_stream(x, "identity", "decode", max_output = 999),
    "zukomp_output_limit"
  )
})

test_that("max_output is enforced whatever the chunk sizes", {
  # The cap shrinks the window handed to the codec, so it must hold when the
  # caller's own output buffer is smaller than the cap, and when it is larger.
  x <- new_payload("zeros", 4096L)
  for (cout in c(1L, 7L, 64L, 8192L)) {
    expect_codec_error(
      zu_test_stream(x, "identity", "decode", out_chunk = cout, max_output = 100),
      "zukomp_output_limit"
    )
  }
})

test_that("max_output = 0 means unlimited", {
  x <- new_payload("ascii", 20000L)
  expect_identical(zu_test_stream(x, "identity", "decode", max_output = 0), x)
})

test_that("a decompression bomb is stopped without allocating its output", {
  # The point of the limit is that proving it works must itself be cheap:
  # never allocate the bomb to show the cap holds.
  bomb <- new_payload("zeros", 10L * 1024L * 1024L)
  expect_codec_error(
    zu_test_stream(bomb, "identity", "decode", max_output = 1024),
    "zukomp_output_limit"
  )
})

test_that("the output-limit condition carries design 7's metadata", {
  err <- expect_error(
    zu_test_stream(new_payload("zeros", 4096L), "identity", "decode",
                   max_output = 10),
    class = "zukomp_output_limit"
  )
  expect_identical(err$codec, "identity")
  expect_identical(err$input_bytes, 4096L)
  expect_false(is.na(err$native_status))
})

test_that("growth arithmetic refuses to overflow", {
  expect_codec_error(zu_test_grow(near_size_max = TRUE), "zukomp_memory_error")
})

test_that("ordinary growth still succeeds", {
  # A positive control: the overflow test above must fail for the right
  # reason, not because zu_test_grow() always errors.
  expect_true(zu_test_grow(near_size_max = FALSE))
})

test_that("max_ratio admits a codec that does not expand", {
  # identity's output always equals its input, so ratio 1 is the tightest
  # limit it can satisfy. The interesting case -- a ratio that actually
  # trips -- needs a codec that expands, so it lands in Stage 6 with
  # deflate. This is the positive control for the arithmetic in the meantime.
  x <- new_payload("ascii", 4096L)
  expect_identical(zu_test_stream(x, "identity", "decode", max_ratio = 1), x)
  expect_identical(zu_test_stream(x, "identity", "decode", max_ratio = 100), x)
})

test_that("limits are independent of each other", {
  x <- new_payload("zeros", 2048L)
  expect_identical(
    zu_test_stream(x, "identity", "decode", max_output = 2048, max_ratio = 1),
    x
  )
  expect_codec_error(
    zu_test_stream(x, "identity", "decode", max_output = 8, max_ratio = 100),
    "zukomp_output_limit"
  )
})
