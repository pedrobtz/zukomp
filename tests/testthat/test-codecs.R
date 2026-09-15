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
      "level_fast", "level_best",
      "detectable", "can_flush", "content_encoding", "source")
  )
  expect_type(d$id, "character")
  expect_type(d$available, "logical")
  expect_type(d$level_min, "integer")
  expect_type(d$can_flush, "logical")
  expect_type(d$source, "character")
})

test_that("can_flush reports the vtable flag, and NA when unavailable", {
  d <- komp_codecs()

  # Every codec this build registers supports flush; the column would be
  # worthless if it could not also say FALSE, so assert the shape rather
  # than only the current values.
  installed <- d[d$available, ]
  expect_false(any(is.na(installed$can_flush)))
  expect_true(all(installed$can_flush[installed$id %in%
    c("identity", "deflate-raw", "zlib", "gzip")]))

  # Declared but not installed: capability is genuinely unknown.
  absent <- d[!d$available, ]
  expect_true(nrow(absent) > 0L)
  expect_true(all(is.na(absent$can_flush)))
})

test_that("a can_flush codec accepts ZU_FLUSH and the table agrees", {
  d <- komp_codecs()
  for (codec in d$id[which(d$available & d$can_encode & d$can_flush)]) {
    x <- new_payload("ascii", 4000)
    z <- zu_test_stream(x, codec, "encode", in_chunk = 512, out_chunk = 512,
                        flush_every = 2)
    expect_identical(zu_test_stream(z, codec, "decode"), x,
                     info = codec)
  }
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

test_that("the codec table caches on registry mutation, not on row count", {
  # The cache used to key on the number of displayed rows, which is not a
  # function of registry state: a satellite implementing a codec zukomp
  # already *declares* flips an existing row from unavailable to available
  # without adding one. A table warmed before that satellite loaded stayed
  # stale, so komp_compress(codec = "zstd") rejected the codec as not
  # installed while komp_codec_available("zstd") said otherwise -- public
  # behaviour that depended on DLL load order.
  #
  # The end-to-end case needs a real second registration and lives in the
  # consumer package. What is checkable here is the key itself.
  gen <- .Call(zukomp_registry_generation)
  expect_type(gen, "double")
  expect_length(gen, 1L)

  # One bump per registered codec, and nothing in this build registers twice.
  expect_identical(gen, as.double(sum(komp_codecs()$available)))

  # Read-only after init: repeated reads never move it.
  d1 <- komp_codecs()
  d2 <- komp_codecs()
  expect_identical(d1, d2)
  expect_identical(.Call(zukomp_registry_generation), gen)
})
