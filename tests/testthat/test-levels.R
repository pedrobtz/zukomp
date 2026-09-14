# The abstract level names of design 4: "fast", "default" and "best".
#
# These are the only cross-codec way to say "compress harder" -- numeric
# levels are codec-native and deliberately not comparable -- so they are the
# one level API codec-agnostic code can use. That is why they have to work on
# every codec, including ones with no level axis.

test_that("all three names are accepted by every encoding codec", {
  d <- komp_codecs()
  x <- new_payload("ascii", 3000)
  for (codec in d$id[which(d$available & d$can_encode)]) {
    for (name in c("fast", "default", "best")) {
      z <- komp_compress(x, codec, level = name)
      expect_identical(komp_decompress(z, codec), x,
                       info = paste(codec, name))
    }
  }
})

test_that('"fast" compresses rather than storing', {
  # The bug this pins: resolving "fast" to level_min gives DEFLATE level 0,
  # which is stored blocks -- output larger than the input. A caller asking
  # to compress cheaply must not get an expansion.
  x <- new_payload("ascii", 8000)
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    z <- komp_compress(x, codec, level = "fast")
    expect_lt(length(z), length(x))
    expect_identical(komp_decompress(z, codec), x, info = codec)
  }
})

test_that('"best" is at least as small as "fast"', {
  x <- new_payload("ascii", 20000)
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    fast <- komp_compress(x, codec, level = "fast")
    best <- komp_compress(x, codec, level = "best")
    expect_lte(length(best), length(fast))
  }
})

test_that("each name resolves to the level the table publishes", {
  d <- komp_codecs()
  x <- new_payload("ascii", 6000)
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    row <- d[d$id == codec, ]
    expect_identical(komp_compress(x, codec, level = "fast"),
                     komp_compress(x, codec, level = row$level_fast),
                     info = codec)
    expect_identical(komp_compress(x, codec, level = "best"),
                     komp_compress(x, codec, level = row$level_best),
                     info = codec)
    # "default" is ZU_LEVEL_DEFAULT, which is what level = NULL sends.
    expect_identical(komp_compress(x, codec, level = "default"),
                     komp_compress(x, codec, level = NULL),
                     info = codec)
  }
})

test_that("a codec with no level axis accepts all three names", {
  # Design 4 as amended: rejecting "fast" here would mean codec-agnostic
  # code still has to special-case the level axis, which is the whole thing
  # the names exist to avoid. All three mean the codec's one behaviour.
  x <- new_payload("ascii", 500)
  for (name in c("fast", "default", "best")) {
    expect_identical(komp_compress(x, "identity", level = name), x,
                     info = name)
  }
})

test_that("an unknown level name is a zukomp condition, not a bare error", {
  x <- new_payload("ascii", 100)
  for (bad in list("fastest", "BEST", "", NA_character_,
                   c("fast", "best"), character(0))) {
    expect_error(komp_compress(x, "gzip", level = bad),
                 class = "zukomp_invalid_argument")
  }
})

test_that("numeric levels still behave exactly as before", {
  x <- new_payload("ascii", 2000)
  expect_identical(komp_decompress(komp_compress(x, "gzip", level = 9L)), x)
  expect_error(komp_compress(x, "gzip", level = 10L),
               class = "zukomp_invalid_argument")
  expect_error(komp_compress(x, "identity", level = 3L),
               class = "zukomp_invalid_argument")
})

test_that("komp_codecs() publishes level_fast and level_best", {
  d <- komp_codecs()

  # DEFLATE: fast is 1, not level_min, because level 0 is stored blocks.
  for (codec in c("deflate-raw", "zlib", "gzip")) {
    row <- d[d$id == codec, ]
    expect_identical(row$level_min, 0L, info = codec)
    expect_identical(row$level_fast, 1L, info = codec)
    expect_identical(row$level_best, row$level_max, info = codec)
  }

  # No level axis, and not installed: both NA, like the rest of the level
  # columns.
  expect_true(is.na(d$level_fast[d$id == "identity"]))
  expect_true(is.na(d$level_best[d$id == "identity"]))
  expect_true(all(is.na(d$level_fast[!d$available])))
})

test_that("a vtable that predates level_fast/level_best still resolves", {
  # The struct_size forward-compatibility contract. Unreachable from the
  # suite otherwise: registration is init-time only, so no short vtable ever
  # reaches the registry -- but a satellite codec compiled against an older
  # header is exactly that case, and a wrong answer here mis-resolves "fast"
  # for every third-party codec.
  expect_identical(zu_test_vtable_levels(0L), c(1L, 9L))

  # Both left zero means "not advertised": fall back to the codec's default.
  expect_identical(zu_test_vtable_levels(1L), c(6L, 6L))

  # Shorter than the appended fields. The bytes are deliberately still set,
  # so a reader that forgets the struct_size guard returns 12345/54321 and
  # fails here rather than passing by luck.
  expect_identical(zu_test_vtable_levels(2L), c(6L, 6L))
})

test_that("zu_codec_get_info() never writes past an older caller's struct", {
  codes <- zu_status_codes()

  full <- zu_test_info_short("gzip", short = FALSE)
  expect_identical(full[[1]], codes[["ZU_OK"]])
  expect_identical(full[[4]], 1L)      # level_fast
  expect_identical(full[[5]], 9L)      # level_best

  # A struct that stops before the appended fields: the prefix is still
  # filled, and the sentinels are returned untouched.
  short <- zu_test_info_short("gzip", short = TRUE)
  expect_identical(short[[1]], codes[["ZU_OK"]])
  expect_identical(short[[2]], 0L)     # level_min, in the required prefix
  expect_identical(short[[3]], 6L)     # level_default, likewise
  expect_identical(short[[4]], -999L)  # untouched sentinel
  expect_identical(short[[5]], -888L)
})
