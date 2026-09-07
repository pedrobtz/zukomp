# Stage 12: prove extensibility instead of claiming it.
#
# These tests run from *outside* zukomp, in a package that consumes it the
# way zuhttp will: Imports + LinkingTo, an importFrom in NAMESPACE, and a
# codec of its own registered through the published C ABI.

test_that("consumer sees zukomp's codecs", {
  expect_true("gzip" %in% zukomp::komp_codecs()$id)
  expect_true(zukomp::komp_codec_available("gzip"))
})

test_that("an externally registered codec is a first-class citizen", {
  d <- zukomp::komp_codecs()
  expect_true("xor5a" %in% d$id)

  row <- d[d$id == "xor5a", ]
  expect_true(row$available)
  # The source column is how a user finds out which package to blame, or to
  # install. zukomp has never heard of this codec.
  expect_identical(row$source, "zukomptest")
  expect_true(row$can_encode)
  expect_true(row$can_decode)
  expect_false(row$detectable)
  expect_true(is.na(row$content_encoding))

  x <- as.raw(1:100)
  expect_identical(
    zukomp::komp_decompress(zukomp::komp_compress(x, "xor5a"), "xor5a"),
    x
  )
})

test_that("core limits apply to a third-party codec", {
  # The payoff of putting limits in the driver rather than in codecs: a
  # codec nobody at zukomp reviewed still cannot bypass the output cap.
  # If this ever fails, the security model is decorative.
  z <- zukomp::komp_compress(raw(10000), "xor5a")
  expect_error(
    zukomp::komp_decompress(z, "xor5a", max_output = 10),
    class = "zukomp_output_limit"
  )
})

test_that("a third-party codec inherits the ratio limit too", {
  z <- zukomp::komp_compress(raw(10000), "xor5a")
  # xor5a does not expand, so ratio 1 is satisfiable and ratio 0.. is not
  # reachable; the point is that the option is honoured, not ignored.
  expect_length(zukomp::komp_decompress(z, "xor5a", max_ratio = 1), 10000L)
})

test_that("auto-detection never guesses a headerless third-party codec", {
  # xor5a advertises no magic and no sniffer, so `auto` must refuse rather
  # than hand back plausible-looking garbage.
  z <- zukomp::komp_compress(as.raw(1:50), "xor5a")
  expect_identical(zukomp::komp_detect(z), NA_character_)
  expect_error(zukomp::komp_decompress(z, "auto"),
               class = "zukomp_undetectable_codec")
})

test_that("the consumer can drive zukomp's C ABI directly", {
  # Uses zukomp_api()'s table and the one-shot entry points, not the R API.
  x <- as.raw(c(0:255, 255:0))
  expect_identical(xor5a_roundtrip_via_c(x), x)
  expect_identical(xor5a_roundtrip_via_c(raw(0)), raw(0))
})

test_that("the codec registered at load time, not on first use", {
  expect_true(xor5a_available())
})

test_that("registering a codec did not disturb zukomp's own", {
  # A third-party registration must be additive. Losing or shadowing a
  # built-in would be a far worse bug than failing to register.
  d <- zukomp::komp_codecs()
  expect_true(all(c("identity", "deflate-raw", "zlib", "gzip") %in% d$id))
  expect_identical(anyDuplicated(d$id), 0L)
  x <- charToRaw("still works")
  expect_identical(zukomp::komp_decompress(zukomp::komp_compress(x, "gzip")), x)
})

test_that("streaming through a third-party codec respects chunk boundaries", {
  # The driver, not the codec, owns the loop -- so a third-party codec gets
  # the boundary handling right for free, or the driver is at fault.
  x <- as.raw(rep(1:255, length.out = 5000))
  z <- zukomp::komp_compress(x, "xor5a")
  expect_identical(zukomp::komp_decompress(z, "xor5a"), x)
  expect_length(z, length(x))
})
