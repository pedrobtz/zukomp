test_that("every failure mode carries design 7's metadata", {
  x <- new_payload("ascii", 1024L)

  expect_codec_error(komp_decompress(as.raw(c(0x1f, 0x8b, 0x08)), "gzip"),
                     "zukomp_truncated")
  expect_codec_error(komp_compress(x, "nope"), "zukomp_unsupported_codec")
  expect_codec_error(komp_decompress(komp_compress(x, "gzip"), "gzip",
                                     max_output = 8),
                     "zukomp_output_limit")
  expect_codec_error(komp_decompress(komp_compress(raw(50000), "gzip"), "gzip",
                                     max_ratio = 2),
                     "zukomp_ratio_limit")
  expect_codec_error(komp_decompress(c(komp_compress(x, "zlib"), as.raw(9)),
                                     "zlib"),
                     "zukomp_trailing_bytes")
})

test_that("an unknown codec reads differently from an uninstalled one", {
  # Two very different problems: a typo, and a codec that ships separately.
  # Collapsing them into one message would send people looking in the
  # wrong place.
  unknown <- tryCatch(komp_compress(raw(1), "nope"), condition = identity)
  absent <- tryCatch(komp_compress(raw(1), "zstd"), condition = identity)
  expect_s3_class(unknown, "zukomp_unsupported_codec")
  expect_s3_class(absent, "zukomp_unsupported_codec")
  expect_match(conditionMessage(unknown), "Unknown codec")
  expect_match(conditionMessage(unknown), "gzip", fixed = TRUE)
  expect_match(conditionMessage(absent), "not installed")
  expect_match(conditionMessage(absent), "separate package")
})

test_that("every zukomp condition inherits from zukomp_error", {
  # The umbrella class is what lets a caller say "anything zukomp raised"
  # without enumerating the hierarchy.
  err <- tryCatch(komp_compress(raw(1), "nope"), condition = identity)
  expect_s3_class(err, "zukomp_error")
  expect_s3_class(err, "error")
})

test_that("the native status travels with the condition", {
  # zuhttp branches on these, so they must be present and truthful.
  err <- tryCatch(komp_decompress(as.raw(c(0x1f, 0x8b, 0x08)), "gzip"),
                  condition = identity)
  expect_identical(err$codec, "gzip")
  expect_identical(err$input_bytes, 3L)
  expect_false(is.na(err$native_status))
  codes <- zukomp:::zu_status_codes()
  expect_identical(err$native_status, unname(codes[["ZU_ERR_TRUNCATED"]]))
})

test_that("checksum, truncation and invalid data are distinguishable", {
  # They arrive at the same place -- a stream that will not decode -- but a
  # caller needs to tell "the network cut me off" from "this is not gzip".
  z <- komp_compress(new_payload("ascii", 1024L), "gzip")
  bad_crc <- z
  bad_crc[length(z) - 7L] <- as.raw(bitwXor(as.integer(bad_crc[length(z) - 7L]), 0xff))

  expect_s3_class(tryCatch(komp_decompress(bad_crc, "gzip"), condition = identity),
                  "zukomp_checksum_error")
  expect_s3_class(tryCatch(komp_decompress(z[1:8], "gzip"), condition = identity),
                  "zukomp_truncated")
  expect_s3_class(tryCatch(komp_decompress(as.raw(1:40), "gzip"), condition = identity),
                  "zukomp_invalid_data")
})

test_that("error messages are stable", {
  # Wording is allowed to change; classes are not. Keeping the wording under
  # snapshot means a reword is one reviewable diff instead of a scatter of
  # broken assertions elsewhere.
  expect_snapshot(error = TRUE, komp_compress(raw(1), codec = "nope"))
  expect_snapshot(error = TRUE, komp_compress(raw(1), codec = "zstd"))
  expect_snapshot(error = TRUE, komp_compress(raw(1), "gzip", level = 99))
  expect_snapshot(error = TRUE, komp_compress("text", "gzip"))
  expect_snapshot(error = TRUE, komp_decompress(as.raw(1:4), "gzip"))
  expect_snapshot(error = TRUE, komp_decompress(komp_compress(raw(5000), "gzip"),
                                                "gzip", max_output = 16))
})
