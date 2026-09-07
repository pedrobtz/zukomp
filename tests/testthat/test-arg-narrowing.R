# R numerics are doubles; the C side takes int32_t, uint32_t and uint64_t.
# Every one of these tests is a value that used to narrow silently into
# something else -- a limit the caller did not ask for, a level that became
# "use the default", or a cast that is undefined behaviour. Found by review.

test_that("a limit beyond integer range is refused, not silently dropped", {
  # max_ratio = 3e9 became NA_integer_, which C read as uint32_t
  # 2147483648: the requested limit vanished and the call succeeded.
  z <- komp_compress(new_payload("zeros", 200000L), "gzip")
  expect_codec_error(komp_decompress(z, "gzip", max_ratio = 3e9),
                     "zukomp_invalid_argument")
  expect_codec_error(komp_decompress(z, "gzip", max_output = 1e300),
                     "zukomp_invalid_argument")
})

test_that("Inf means no limit, for both caps", {
  x <- new_payload("zeros", 200000L)
  z <- komp_compress(x, "gzip")
  expect_identical(komp_decompress(z, "gzip", max_output = Inf), x)
  expect_identical(komp_decompress(z, "gzip", max_ratio = Inf), x)
  # 0 keeps meaning the same thing
  expect_identical(komp_decompress(z, "gzip", max_output = 0), x)
})

test_that("limits that are in range still bite", {
  # The positive control: refusing bad input must not have loosened the
  # limits that matter.
  z <- komp_compress(new_payload("zeros", 200000L), "gzip")
  expect_codec_error(komp_decompress(z, "gzip", max_output = 1024),
                     "zukomp_output_limit")
  expect_codec_error(komp_decompress(z, "gzip", max_ratio = 10),
                     "zukomp_ratio_limit")
  # A cap above integer range is a real cap, not NA: 2^31 comfortably
  # exceeds this 200 KB output, so it must simply be honoured.
  expect_length(komp_decompress(z, "gzip",
                                max_output = .Machine$integer.max + 1),
                200000L)
})

test_that("a level beyond integer range is a zukomp condition, not an R error", {
  # `level != as.integer(level)` is NA out of range, so the `if` failed
  # with "missing value where TRUE/FALSE needed" -- breaking the design 7
  # contract that every failure carries a class and metadata.
  for (bad in list(1e10, -1e10, Inf, -Inf, NaN)) {
    expect_codec_error(komp_compress(raw(10), "gzip", level = bad),
                       "zukomp_invalid_argument")
  }
})

test_that("an unrepresentable level is never honoured as the default", {
  # NA_INTEGER and ZU_LEVEL_DEFAULT are both INT_MIN, so a level that
  # failed to narrow used to be indistinguishable from "codec default" --
  # the caller believes they asked for level 9 and get level 6.
  x <- new_payload("ascii", 8192L)
  expect_codec_error(zu_test_stream(x, "gzip", "encode", level = 1e10),
                     "zukomp_invalid_argument")
  # ...while a real level still reaches the codec. The payload matters:
  # helper-corpus's "ascii" is repetitive enough that levels 1 and 9 agree
  # exactly, which would make this assertion vacuous.
  varied <- charToRaw(strrep("the quick brown fox jumps over the lazy dog ", 400))
  expect_lt(length(zu_test_stream(varied, "gzip", "encode", level = 9L)),
            length(zu_test_stream(varied, "gzip", "encode", level = 1L)))
  expect_identical(zu_test_stream(x, "gzip", "encode", level = NULL),
                   zu_test_stream(x, "gzip", "encode"))
})

test_that("codec = auto is refused when compressing", {
  # "auto" is a decompression instruction, not a codec. Reaching the
  # registry produced "not available", which reads as "install a satellite
  # package" for something that is not a compression option at all.
  err <- expect_error(komp_compress(raw(10), "auto"),
                      class = "zukomp_invalid_argument")
  expect_match(conditionMessage(err), "decompress")
  expect_false(grepl("not available", conditionMessage(err), fixed = TRUE))
  # ...and it still works where it belongs
  expect_identical(komp_decompress(komp_compress(raw(10), "gzip"), "auto"),
                   raw(10))
})

test_that("malformed limit and level arguments are all classed conditions", {
  z <- komp_compress(raw(100), "gzip")
  for (bad in list(NA_real_, -1, c(1, 2), "big", TRUE)) {
    expect_codec_error(komp_decompress(z, "gzip", max_output = bad),
                       "zukomp_invalid_argument")
  }
  for (bad in list(NA_integer_, -1, c(1, 2), "9")) {
    expect_codec_error(komp_compress(raw(10), "gzip", level = bad),
                       "zukomp_invalid_argument")
  }
})
