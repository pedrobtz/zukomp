# zukomp's zstd and lz4 codecs (#55), from outside zukomp and through the C
# table only -- the path zuarrow takes for Arrow IPC bodies (ZSTD,
# LZ4_FRAME) and qio for Parquet pages (ZSTD, LZ4_RAW). A body is compressed
# in one shot at a level, stored with its decoded length, and decompressed
# in one shot into a buffer of that length, with the length passed as
# ZU_DEC_EXPECTED_SIZE and max_output binding.

test_that("the three codecs are in the table with their reserved ids", {
  ids <- c(zstd = 17L, "lz4-frame" = 18L, "lz4-block" = 19L)
  for (codec in names(ids)) {
    lv <- codec_levels_via_c(codec)
    expect_false(is.null(lv), info = codec)
    expect_identical(lv[["id"]], ids[[codec]], info = codec)
  }
  expect_identical(codec_levels_via_c("zstd")[c("min", "max", "default")],
                   c(min = -131072L, max = 22L, default = 3L))
  expect_identical(codec_levels_via_c("lz4-frame")[c("fast", "best")],
                   c(fast = 1L, best = 12L))
})

test_that("a body round-trips one shot each way, told its size", {
  x <- as.raw(rep(c(1:200, 50:1), 400))
  for (codec in c("zstd", "lz4-frame", "lz4-block")) {
    for (level in list(NULL, 1L, 9L)) {
      z <- compress_one_via_c(x, codec, level)
      expect_identical(z$status, "ZU_OK", info = codec)
      got <- decompress_one_via_c(z$bytes, codec, cap = length(x),
                                  max_output = 2^30,
                                  expected_size = length(x))
      expect_identical(got$status, "ZU_OK", info = codec)
      expect_identical(got$bytes, x, info = codec)
      # And zukomp's own R decoder agrees with the C one.
      expect_identical(zukomp::komp_decompress(z$bytes, codec), x, info = codec)
    }
  }
})

test_that("core limits bind on zukomp's own codecs through the table", {
  bomb <- raw(2^20)
  for (codec in c("zstd", "lz4-frame", "lz4-block")) {
    z <- compress_one_via_c(bomb, codec)$bytes
    expect_lt(length(z), 5000L)
    # max_output tighter than the recorded size: the limit refuses the body.
    got <- decompress_one_via_c(z, codec, cap = 2^20, max_output = 4096,
                                expected_size = 2^20)
    expect_identical(got$status, "ZU_ERR_OUTPUT_LIMIT", info = codec)
    expect_lte(length(got$bytes), 4096L)
    # And with no size recorded at all.
    got <- decompress_one_via_c(z, codec, cap = 2^20, max_output = 4096)
    expect_identical(got$status, "ZU_ERR_OUTPUT_LIMIT", info = codec)
  }
})

test_that("a body whose recorded size lies is invalid data", {
  x <- as.raw(rep(1:250, 100))
  for (codec in c("zstd", "lz4-frame", "lz4-block")) {
    z <- compress_one_via_c(x, codec)$bytes
    short <- decompress_one_via_c(z, codec, cap = length(x),
                                  expected_size = length(x) - 1)
    expect_identical(short$status, "ZU_ERR_INVALID_DATA", info = codec)
    long <- decompress_one_via_c(z, codec, cap = length(x) + 10,
                                 expected_size = length(x) + 1)
    expect_identical(long$status, "ZU_ERR_INVALID_DATA", info = codec)
  }
})

test_that("a buffer too small for the body is an output limit", {
  x <- as.raw(rep(1:250, 100))
  for (codec in c("zstd", "lz4-frame", "lz4-block")) {
    z <- compress_one_via_c(x, codec)$bytes
    got <- decompress_one_via_c(z, codec, cap = length(x) - 1)
    expect_identical(got$status, "ZU_ERR_OUTPUT_LIMIT", info = codec)
  }
})

test_that("a consumer built against the original header still works", {
  # zu_decoder_opts gained expected_size after 0.1.0. A consumer compiled
  # against the old header sets the old struct_size and never the new flag,
  # and decodes exactly as before; claiming the flag without the field is a
  # caller bug, refused rather than read as "expected size 0".
  x <- as.raw(rep(1:250, 100))
  z <- compress_one_via_c(x, "zstd")$bytes
  got <- decompress_one_via_c(z, "zstd", cap = length(x), old_struct = TRUE)
  expect_identical(got$status, "ZU_OK")
  expect_identical(got$bytes, x)
  bad <- decompress_one_via_c(z, "zstd", cap = length(x),
                              expected_size = length(x), old_struct = TRUE)
  expect_identical(bad$status, "ZU_ERR_INVALID_ARGUMENT")
})

test_that("truncated and corrupted bodies fail cleanly through the table", {
  x <- as.raw(rep(1:250, 100))
  for (codec in c("zstd", "lz4-frame")) {
    z <- compress_one_via_c(x, codec)$bytes
    cut <- decompress_one_via_c(z[seq_len(length(z) - 3L)], codec,
                                cap = length(x), expected_size = length(x))
    expect_identical(cut$status, "ZU_ERR_TRUNCATED", info = codec)
    bad <- z
    bad[length(bad)] <- as.raw(bitwXor(as.integer(bad[length(bad)]), 1L))
    sum <- decompress_one_via_c(bad, codec, cap = length(x),
                                expected_size = length(x))
    expect_identical(sum$status, "ZU_ERR_CHECKSUM", info = codec)
  }
})
