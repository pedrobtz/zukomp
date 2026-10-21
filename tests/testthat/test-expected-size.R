# ZU_DEC_EXPECTED_SIZE: the caller knows the decoded size from outside the
# stream (an Arrow IPC body's length prefix, a Parquet page header) and the
# core holds every codec to it, like max_output (#55). Output beyond it, or a
# stream ending short of it, is invalid data; where max_output is tighter,
# max_output binds as usual.

test_that("the exact size decodes, for every codec", {
  x <- new_payload("structured", 5000L)
  for (codec in komp_codecs()$id[komp_codecs()$available]) {
    z <- komp_compress(x, codec)
    for (n in c(1L, 7L, 4096L)) {
      expect_identical(
        zu_test_stream(z, codec, "decode", in_chunk = n, out_chunk = n,
                       expected_size = length(x)),
        x, info = paste(codec, "chunk", n))
    }
    expect_identical(
      zu_test_decompress_one(z, codec, length(x), expected_size = length(x)),
      x, info = codec)
  }
})

test_that("a stream longer than promised is invalid data", {
  x <- new_payload("structured", 5000L)
  for (codec in komp_codecs()$id[komp_codecs()$available]) {
    z <- komp_compress(x, codec)
    expect_codec_error(
      zu_test_stream(z, codec, "decode", expected_size = length(x) - 1),
      "zukomp_invalid_data", info = codec)
    expect_codec_error(
      zu_test_decompress_one(z, codec, length(x), expected_size = 10),
      "zukomp_invalid_data", info = codec)
  }
})

test_that("a stream shorter than promised is invalid data", {
  x <- new_payload("structured", 5000L)
  for (codec in komp_codecs()$id[komp_codecs()$available]) {
    z <- komp_compress(x, codec)
    for (n in c(1L, 4096L)) {
      expect_codec_error(
        zu_test_stream(z, codec, "decode", in_chunk = n, out_chunk = n,
                       expected_size = length(x) + 1),
        "zukomp_invalid_data", info = paste(codec, "chunk", n))
    }
    expect_codec_error(
      zu_test_decompress_one(z, codec, length(x) + 100,
                             expected_size = length(x) + 1),
      "zukomp_invalid_data", info = codec)
  }
})

test_that("max_output still binds when it is tighter than the size", {
  x <- new_payload("structured", 5000L)
  for (codec in c("gzip", "zstd", "lz4-frame", "lz4-block")) {
    z <- komp_compress(x, codec)
    expect_codec_error(
      zu_test_stream(z, codec, "decode", max_output = 1000,
                     expected_size = length(x)),
      "zukomp_output_limit", info = codec)
  }
})

test_that("an empty payload has expected size 0, which is not 'unknown'", {
  for (codec in c("gzip", "zstd", "lz4-frame", "lz4-block")) {
    z <- komp_compress(raw(0), codec)
    expect_identical(zu_test_stream(z, codec, "decode", expected_size = 0),
                     raw(0), info = codec)
    expect_identical(zu_test_decompress_one(z, codec, 0, expected_size = 0),
                     raw(0), info = codec)
    z1 <- komp_compress(as.raw(1), codec)
    expect_codec_error(zu_test_stream(z1, codec, "decode", expected_size = 0),
                       "zukomp_invalid_data", info = codec)
  }
})

test_that("lz4-block decodes in one pass when told the size", {
  # The case the field exists for: a bare block carries no length, so with
  # the size the decoder writes exactly that many bytes; and a block that
  # lies about its contents fails rather than being searched for.
  x <- new_payload("ascii", 100000L)
  z <- komp_compress(x, "lz4-block")
  expect_identical(zu_test_decompress_one(z, "lz4-block", length(x),
                                          expected_size = length(x)), x)
  # Into a larger sink, through the staging path, and a byte at a time.
  expect_identical(zu_test_decompress_one(z, "lz4-block", length(x) + 10,
                                          expected_size = length(x)), x)
  expect_identical(zu_test_stream(z, "lz4-block", "decode", in_chunk = 1L,
                                  out_chunk = 1L, expected_size = length(x)),
                   x)
  # A size beyond max_output is refused before anything is allocated.
  expect_codec_error(
    zu_test_stream(z, "lz4-block", "decode", max_output = 10,
                   expected_size = 2^40),
    "zukomp_output_limit")
  # A lying size is invalid data in both directions.
  expect_codec_error(
    zu_test_decompress_one(z, "lz4-block", length(x),
                           expected_size = length(x) - 1),
    "zukomp_invalid_data")
  expect_codec_error(
    zu_test_stream(z, "lz4-block", "decode", expected_size = 2^31),
    "zukomp_invalid_data")
})

test_that("expected_size is validated before it reaches C", {
  z <- komp_compress(raw(10), "gzip")
  for (bad in list(-1, 1.5, NA_real_, Inf, c(1, 2), "10")) {
    expect_codec_error(zu_test_stream(z, "gzip", "decode", expected_size = bad),
                       "zukomp_invalid_argument")
  }
})
