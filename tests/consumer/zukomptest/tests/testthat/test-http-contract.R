# Stage 15: the zuhttp integration contract (design 16).
#
# zuhttp does not exist yet, so what is proved here is the half that
# concerns zukomp: that an HTTP client can build its Accept-Encoding from
# the registry, resolve content-coding tokens, chain decoders, apply the
# deflate-ambiguity policy, and decode a body incrementally -- all through
# the published C ABI, with no vendored symbol anywhere in sight.

test_that("Accept-Encoding follows zukomp's registry", {
  ae <- default_accept_encoding()
  expect_match(ae, "gzip")
  expect_match(ae, "deflate")
  # `br` appears the day zukomp.brotli is installed, with no change to
  # zuhttp. Until then its absence is the evidence the header is derived.
  expect_false(grepl("br", ae, fixed = TRUE))
})

test_that("Accept-Encoding advertises only what can actually be decoded", {
  tokens <- trimws(strsplit(default_accept_encoding(), ",")[[1]])
  d <- zukomp::komp_codecs()
  for (tok in tokens) {
    row <- d[!is.na(d$content_encoding) & d$content_encoding == tok, ]
    expect_identical(nrow(row), 1L, info = tok)
    expect_true(row$available, info = tok)
    expect_true(row$can_decode, info = tok)
  }
  # deflate-raw has no content-coding token, so it must never be advertised
  expect_false("deflate-raw" %in% tokens)
  # identity is always acceptable; advertising it is noise
  expect_false("identity" %in% tokens)
})

test_that("content-coding tokens resolve through zukomp, case-insensitively", {
  expect_identical(codec_for_token("gzip"), "gzip")
  expect_identical(codec_for_token("GZIP"), "gzip")
  # design 3: the HTTP token `deflate` means zlib
  expect_identical(codec_for_token("deflate"), "zlib")
  expect_identical(codec_for_token("Deflate"), "zlib")
  expect_identical(codec_for_token("br"), NA_character_)
  expect_identical(codec_for_token("nonsense"), NA_character_)
})

test_that("a gzip response body decodes", {
  x <- charToRaw(strrep("response body ", 500))
  expect_identical(decode_body(zukomp::komp_compress(x, "gzip"), "gzip"), x)
})

test_that("a deflate response decodes in both flavours", {
  # The ambiguity design 3 warns about: some servers send zlib under the
  # `deflate` token, some send headerless DEFLATE. zukomp resolves the token
  # to zlib and refuses to guess further; the retry is this client's policy.
  x <- charToRaw(strrep("ambiguous ", 300))
  expect_identical(decode_body(zukomp::komp_compress(x, "zlib"), "deflate"), x)
  expect_identical(decode_body(zukomp::komp_compress(x, "deflate-raw"), "deflate"), x)
})

test_that("the raw-DEFLATE retry does not become a second chance at a bomb", {
  # The retry fires only on invalid data, never on a limit. Retrying after
  # zukomp_output_limit would let a hostile server bypass the cap simply by
  # labelling its body `deflate`.
  bomb <- zukomp::komp_compress(raw(2e6), "zlib")
  expect_error(
    decode_body(bomb, "deflate", max_decompressed_bytes = 4096),
    class = "zukomp_output_limit"
  )
})

test_that("multiple content-codings are decoded right to left", {
  # `Content-Encoding: gzip, deflate` means gzip was applied first, so
  # deflate must be undone first. Getting the order backwards produces
  # garbage rather than an error, which is why it is tested directly.
  x <- charToRaw(strrep("chained ", 400))
  body <- zukomp::komp_compress(zukomp::komp_compress(x, "gzip"), "zlib")
  expect_identical(decode_body(body, "gzip, deflate"), x)

  triple <- zukomp::komp_compress(body, "gzip")
  expect_identical(decode_body(triple, "gzip, deflate, gzip"), x)
})

test_that("identity in a coding chain is a no-op", {
  x <- charToRaw("plain")
  expect_identical(decode_body(x, "identity"), x)
  expect_identical(decode_body(zukomp::komp_compress(x, "gzip"), "identity, gzip"), x)
})

test_that("an absurd coding chain is refused before decoding anything", {
  x <- zukomp::komp_compress(charToRaw("x"), "gzip")
  expect_error(decode_body(x, paste(rep("gzip", 40), collapse = ", ")),
               "more than the")
})

test_that("an unknown content-coding is refused, not ignored", {
  x <- zukomp::komp_compress(charToRaw("x"), "gzip")
  expect_error(decode_body(x, "br"), "Unsupported Content-Encoding")
})

test_that("limits are set by the client and enforced by zukomp", {
  # design 16 point 4: max_decompressed_bytes and max_decompression_ratio
  # map straight onto zu_decoder_opts, and the driver -- not this package --
  # is what stops the stream.
  bomb <- zukomp::komp_compress(raw(10e6), "gzip")
  expect_lt(length(bomb), 15000L)
  expect_error(decode_body(bomb, "gzip", max_decompressed_bytes = 4096),
               class = "zukomp_output_limit")
  expect_error(decode_body(bomb, "gzip", max_decompression_ratio = 10),
               class = "zukomp_ratio_limit")
})

test_that("a bomb cannot be laundered through an intermediate coding", {
  # Limits apply per stage. A chain where each step looks modest but the
  # whole expands enormously must still be stopped.
  bomb <- zukomp::komp_compress(zukomp::komp_compress(raw(10e6), "gzip"), "zlib")
  expect_error(decode_body(bomb, "gzip, deflate", max_decompressed_bytes = 4096),
               class = "zukomp_error")
})

test_that("a response body decodes incrementally, never held whole", {
  # design 24 criterion 11, in the form provable without an HTTP client:
  # 5 MB of decoded output through a 4 KiB sink that is reused, so peak
  # allocation is the sink and not the body.
  x <- raw(5e6)
  body <- zukomp::komp_compress(x, "gzip")
  expect_lt(length(body), 20000L)

  res <- decode_incremental(body, "gzip", chunk = 4096L)
  expect_true(res$ok)
  expect_identical(res$bytes, 5e6)

  # Same answer at every sink size: the chunking is invisible in the result.
  for (chunk in c(1L, 7L, 4096L, 65536L)) {
    r <- decode_incremental(body, "gzip", chunk = chunk)
    expect_true(r$ok, info = paste("chunk", chunk))
    expect_identical(r$bytes, 5e6, info = paste("chunk", chunk))
    expect_identical(r$checksum, res$checksum, info = paste("chunk", chunk))
  }
})

test_that("incremental decoding honours the limits too", {
  body <- zukomp::komp_compress(raw(5e6), "gzip")
  res <- decode_incremental(body, "gzip", chunk = 4096L, max_output = 8192)
  expect_false(res$ok)
  expect_match(res$status, "output size limit")
})

test_that("incremental decoding of R's memCompress output works", {
  # A second implementation's bytes, decoded incrementally through the ABI.
  x <- charToRaw(strrep("interop ", 1000))
  res <- decode_incremental(memCompress(x, "gzip"), "zlib", chunk = 512L)
  expect_true(res$ok)
  expect_identical(res$bytes, as.double(length(x)))
})

test_that("no vendored codec symbol appears anywhere in this package", {
  # design 24 criterion 14, from the consumer's side: zuhttp must never see
  # miniz. If this fails, zukomp's header has leaked something.
  src <- list.files(test_path("..", ".."), pattern = "\\.[ch]$",
                    recursive = TRUE, full.names = TRUE)
  skip_if(length(src) == 0L, "sources not present in an installed package")
  for (f in src) {
    text <- readLines(f, warn = FALSE)
    for (pattern in c("miniz", "mz_", "tdefl", "tinfl")) {
      expect_length(grep(pattern, text, fixed = TRUE, value = TRUE), 0L)
    }
  }
})
