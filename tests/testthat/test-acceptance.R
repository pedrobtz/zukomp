# design 24's acceptance criteria, as executable assertions.
#
# Most are already covered in depth by the files named after each concern;
# this file is the index that says so, and fails loudly if a criterion stops
# being met. Criteria that cannot be checked from inside R -- cross-platform
# builds, sanitizer runs -- name where they are checked instead.

test_that("criterion 2: the four codecs round-trip at every valid level", {
  # The per-codec files test level extremes; this is the exhaustive version.
  skip_if_no_slow_tests()
  for (codec in c("identity", "deflate-raw", "zlib", "gzip")) {
    row <- komp_codecs()[komp_codecs()$id == codec, ]
    levels <- if (is.na(row$level_min)) list(NULL) else as.list(row$level_min:row$level_max)
    for (kind in payload_kinds()) {
      withr::local_seed(5L)
      x <- new_payload(kind, 4096L)
      for (lvl in levels) {
        expect_identical(
          komp_decompress(komp_compress(x, codec, level = lvl), codec), x,
          info = sprintf("%s / %s / level %s", codec, kind,
                         if (is.null(lvl)) "default" else lvl)
        )
      }
    }
  }
})

test_that("criterion 3: external encoders' output is accepted here", {
  # The reverse direction -- external decoders accepting our output -- is
  # tools/check-interop.sh, a CI job, because CRAN guarantees no gzip.
  m <- fixture_manifest()
  expect_gt(nrow(m), 20L)
  expect_gt(length(unique(m$generator)), 1L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    expect_identical(
      komp_decompress(fixture_bytes(row$codec, row$file), row$codec),
      fixture_plaintext(row),
      info = paste(row$codec, row$file, row$generator)
    )
  }
})

test_that("criterion 4: streaming is correct at one-byte boundaries both ways", {
  x <- new_payload("structured", 5003L)
  for (codec in c("identity", "deflate-raw", "zlib", "gzip")) {
    expect_chunked_roundtrip(x, codec, 1L, 1L)
  }
})

test_that("criterion 6: every checksum corruption is a checksum error", {
  x <- new_payload("ascii", 2048L)
  for (codec in c("zlib", "gzip")) {
    z <- komp_compress(x, codec)
    trailer <- if (identical(codec, "zlib")) 4L else 8L
    for (off in seq_len(trailer) - 1L) {
      bad <- z
      i <- length(z) - off
      bad[i] <- as.raw(bitwXor(as.integer(bad[i]), 0xff))
      expect_codec_error(komp_decompress(bad, codec), "zukomp_checksum_error",
                         info = sprintf("%s trailer -%d", codec, off))
    }
  }
})

test_that("criterion 7: both limits stop decompression with the right class", {
  z <- komp_compress(new_payload("zeros", 100000L), "gzip")
  expect_codec_error(komp_decompress(z, "gzip", max_output = 1024),
                     "zukomp_output_limit")
  expect_codec_error(komp_decompress(z, "gzip", max_ratio = 5),
                     "zukomp_ratio_limit")
  # Deterministic: the same input and limits give the same answer every time.
  for (i in 1:5) {
    expect_codec_error(komp_decompress(z, "gzip", max_output = 1024),
                       "zukomp_output_limit")
  }
})

test_that("criterion 9: no vendored type or symbol appears in zukomp.h", {
  header <- installed_header_code()
  for (pattern in c("miniz", "mz_", "tdefl", "tinfl", "MZ_")) {
    expect_length(grep(pattern, header, fixed = TRUE, value = TRUE), 0L)
  }
})

test_that("criterion 12: adding a codec needs no header change and no ABI bump", {
  # The consumer package registers xor5a at ZU_CODEC_VENDOR_BASE without
  # zukomp.h changing at all. What can be checked from here is the shape
  # that makes it possible: codecs are enum values discovered at runtime,
  # and the vendor range is reserved in the published header.
  header <- installed_header_code()
  expect_gt(length(grep("ZU_CODEC_VENDOR_BASE", header, fixed = TRUE)), 0L)
  expect_gt(length(grep("zu_register_codec", header, fixed = TRUE)), 0L)
  expect_gt(length(grep("zu_codec_list", header, fixed = TRUE)), 0L)
  expect_identical(zu_abi_version(), 1L)
})

test_that("criterion 14: no archive, ZIP or PNG symbol is reachable", {
  syms <- exported_symbols()
  # The archive API is uniformly mz_zip_*, and the PNG writer is
  # tdefl_write_image_*. Both are removed by the Makevars define set, the
  # PNG one via our own patch.
  for (pattern in c("mz_zip", "tdefl_write_image")) {
    expect_identical(grep(pattern, syms, value = TRUE), character(0),
                     info = pattern)
  }
  # Deliberately NOT banned: tdefl_create_comp_flags_from_zip_params, whose
  # name mentions zip but which only maps compression parameters to tdefl
  # flags. Banning on the substring rather than on what the symbol does
  # would be a test that looks strict and means nothing.
  expect_gt(length(grep("tinfl_decompress", syms)), 0L)
})

test_that("the MVP surface from design 23 is present and exported", {
  exported <- getNamespaceExports("zukomp")
  for (fn in c("komp_compress", "komp_decompress", "komp_detect",
               "komp_codecs", "komp_codec_available", "komp_info")) {
    expect_true(fn %in% exported, info = fn)
  }
  # ...and nothing codec-specific leaked into a public name (design 1).
  expect_length(grep("^(deflate|inflate|gzip|zlib|miniz)", exported), 0L)
})

test_that("the registry, not DEFLATE, is the product", {
  # design 25's framing, as an assertion: every codec is reached the same
  # way, and no built-in gets a privileged entry point of its own.
  exported <- getNamespaceExports("zukomp")
  expect_length(grep("^komp_(compress|decompress)_", exported), 0L)
  d <- komp_codecs()
  expect_gt(nrow(d), 4L)
  expect_true(all(c("id", "available", "source") %in% names(d)))
})
