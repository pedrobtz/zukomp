test_that("identity is registered and fully described", {
  d <- komp_codecs()
  row <- d[d$id == "identity", ]

  expect_identical(nrow(row), 1L)
  expect_true(row$available)
  expect_identical(row$source, "zukomp")
  expect_true(row$can_encode)
  expect_true(row$can_decode)
  expect_identical(row$content_encoding, "identity")
})

test_that("identity is deliberately not detectable", {
  # Pass-through bytes are indistinguishable from arbitrary bytes, so `auto`
  # resolving to identity would mean silently returning the input unchanged.
  d <- komp_codecs()
  expect_false(d$detectable[d$id == "identity"])
})

test_that("identity reports no level axis", {
  d <- komp_codecs()
  row <- d[d$id == "identity", ]
  expect_true(is.na(row$level_min))
  expect_true(is.na(row$level_max))
  expect_true(is.na(row$level_default))
})

test_that("komp_codecs() has exactly the columns design 6 specifies", {
  d <- komp_codecs()
  expect_s3_class(d, "data.frame")
  expect_identical(
    names(d),
    c("id", "available", "can_encode", "can_decode",
      "level_min", "level_max", "level_default",
      "detectable", "content_encoding", "source")
  )
  expect_type(d$id, "character")
  expect_type(d$available, "logical")
  expect_type(d$level_min, "integer")
  expect_type(d$source, "character")
})

test_that("every declared codec appears, installed or not", {
  # Values are permanent (design 3): a codec that is not compiled in keeps
  # its name and reports unavailable, so callers can tell "not installed"
  # from "does not exist".
  d <- komp_codecs()
  expect_true(all(
    c("identity", "deflate-raw", "zlib", "gzip",
      "brotli", "zstd", "lz4-frame", "lz4-block",
      "snappy-frame", "snappy-raw") %in% d$id
  ))
  expect_identical(anyDuplicated(d$id), 0L)
})

test_that("declared-but-absent codecs report unavailable", {
  expect_false(komp_codec_available("zstd"))
  expect_false(komp_codec_available("brotli"))
})

test_that("an unavailable codec has NA, not FALSE, for its capabilities", {
  d <- komp_codecs()
  row <- d[d$id == "zstd", ]
  expect_false(row$available)
  expect_true(is.na(row$can_encode))
  expect_true(is.na(row$can_decode))
  expect_true(is.na(row$detectable))
  expect_true(is.na(row$source))
  # ...but the HTTP token is a property of the name, not the implementation.
  expect_identical(row$content_encoding, "zstd")
})

test_that("unknown codecs are a clean error, not a crash", {
  expect_codec_error(komp_codec_available("nope"), "zukomp_unsupported_codec")
})

test_that("the unknown-codec error names the codec and lists the known ones", {
  err <- expect_error(komp_codec_available("nope"),
                      class = "zukomp_unsupported_codec")
  expect_identical(err$codec, "nope")
  expect_match(conditionMessage(err), "identity", fixed = TRUE)
})

test_that("komp_codec_available() rejects malformed input", {
  expect_error(komp_codec_available(42), class = "zukomp_invalid_argument")
  expect_error(komp_codec_available(c("a", "b")), class = "zukomp_invalid_argument")
  expect_error(komp_codec_available(NA_character_), class = "zukomp_invalid_argument")
})

test_that("content-coding tokens map to codecs, not the other way round", {
  # design 3: `Content-Encoding: deflate` means zlib here. The retry-as-raw
  # policy for servers that send headerless DEFLATE lives in zuhttp.
  d <- komp_codecs()
  expect_identical(d$content_encoding[d$id == "zlib"], "deflate")
  expect_identical(d$content_encoding[d$id == "gzip"], "gzip")
  expect_identical(d$content_encoding[d$id == "brotli"], "br")
  # headerless formats are not content-codings
  expect_true(is.na(d$content_encoding[d$id == "deflate-raw"]))
})
