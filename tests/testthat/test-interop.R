# Decodes bytes produced by other implementations. The fixtures are
# committed rather than generated here: CRAN guarantees neither python3 nor
# a system gzip, and a test that silently skips is worse than no test.
# tools/make-fixtures.R regenerates them; see tests/testthat/fixtures/.

test_that("decodes zlib produced by external encoders", {
  m <- fixture_manifest("zlib")
  expect_gt(nrow(m), 0L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    got <- zu_test_stream(fixture_bytes("zlib", row$file), "zlib", "decode")
    expect_identical(got, fixture_plaintext(row),
                     info = paste(row$file, "from", row$generator))
  }
})

test_that("decodes raw DEFLATE produced by external encoders", {
  m <- fixture_manifest("deflate-raw")
  expect_gt(nrow(m), 0L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    got <- zu_test_stream(fixture_bytes("deflate-raw", row$file),
                          "deflate-raw", "decode")
    expect_identical(got, fixture_plaintext(row),
                     info = paste(row$file, "from", row$generator))
  }
})

test_that("external fixtures decode at pathological chunk sizes too", {
  # Interop and streaming are separate risks: a wrapper can be parsed
  # correctly in one pass and still break when its header spans two calls.
  m <- fixture_manifest("zlib")
  m <- m[m$payload == "ascii", , drop = FALSE]
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    got <- zu_test_stream(fixture_bytes("zlib", row$file), "zlib", "decode",
                          in_chunk = 1L, out_chunk = 1L)
    expect_identical(got, fixture_plaintext(row), info = row$file)
  }
})

test_that("R's own memCompress output decodes as zlib", {
  # Worth an explicit test because memCompress(type = "gzip") emits zlib
  # format, not gzip, and that surprises people.
  m <- fixture_manifest("zlib")
  m <- m[grepl("memcompress", m$file), , drop = FALSE]
  expect_gt(nrow(m), 0L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    expect_identical(
      zu_test_stream(fixture_bytes("zlib", row$file), "zlib", "decode"),
      fixture_plaintext(row)
    )
  }
})

test_that("zukomp output is accepted by R's memDecompress", {
  # The reverse direction: our bytes must be readable by someone else's
  # decoder, not just our own.
  x <- new_payload("ascii", 4096L)
  for (lvl in c(0L, 6L, 9L)) {
    z <- zu_test_stream(x, "zlib", "encode", level = lvl)
    expect_identical(memDecompress(z, "gzip"), x, info = paste("level", lvl))
  }
})

test_that("decodes lz4-block produced by an external encoder", {
  # #70. Until these fixtures, lz4-block was decoded only from zukomp's own
  # encoder and from hand-built blocks, and an encoder and decoder from one
  # build can agree on a mistake. The blocks come from python-lz4, which
  # bundles its own liblz4 release: the default, fast and HC compressors
  # over every payload kind (tools/make-fixtures.py). A bare block does not
  # record its decoded size, so each is decoded both told the size
  # (ZU_DEC_EXPECTED_SIZE, design section 8, decision 19) and not told it,
  # which is the size search in src/codec_lz4.c; and at every chunk size,
  # which is its input gather.
  m <- fixture_manifest("lz4-block")
  expect_gt(nrow(m), 0L)
  expect_true(all(c("ascii", "zeros", "utf8", "lcg", "structured", "empty")
                  %in% m$payload))
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    blk <- fixture_bytes("lz4-block", row$file)
    want <- fixture_plaintext(row)
    expect_identical(komp_decompress(blk, "lz4-block"), want, info = row$file)
    for (n in chunk_sizes()) {
      info <- paste(row$file, "chunk", n)
      expect_identical(
        zu_test_stream(blk, "lz4-block", "decode", in_chunk = n,
                       out_chunk = n),
        want, info = info
      )
      expect_identical(
        zu_test_stream(blk, "lz4-block", "decode", in_chunk = n,
                       out_chunk = n, expected_size = length(want)),
        want, info = info
      )
    }
  }
})
