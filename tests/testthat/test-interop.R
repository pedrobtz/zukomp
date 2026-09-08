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
