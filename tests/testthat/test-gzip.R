test_that("parses every optional header field", {
  # The gzip header is the most likely place in the package for a parser
  # bug: variable length, four optional fields, two of them NUL-terminated
  # and attacker-controlled. Each gets a fixture from an external encoder.
  m <- fixture_manifest("gzip")
  expect_gt(nrow(m), 0L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    expect_identical(
      zu_test_stream(fixture_bytes("gzip", row$file), "gzip", "decode"),
      fixture_plaintext(row),
      info = paste(row$file, "from", row$generator)
    )
  }
})

test_that("optional header fields parse when split across calls", {
  # A header field can straddle any number of process() calls. Parsing it in
  # one pass and parsing it a byte at a time are different code paths.
  for (f in c("py_fextra_ascii.bin", "py_fname_ascii.bin",
              "py_fcomment_ascii.bin", "py_fhcrc_ascii.bin",
              "py_allflags_ascii.bin")) {
    expect_identical(
      zu_test_stream(fixture_bytes("gzip", f), "gzip", "decode",
                     in_chunk = 1L, out_chunk = 1L),
      new_payload("ascii", 4096L),
      info = f
    )
  }
})

test_that("round-trips the payload corpus at every level extreme", {
  info <- komp_codecs()[komp_codecs()$id == "gzip", ]
  for (kind in payload_kinds()) {
    withr::local_seed(3L)
    x <- new_payload(kind, 8192L)
    for (lvl in c(info$level_min, info$level_default, info$level_max)) {
      expect_roundtrip(x, "gzip", lvl)
    }
  }
})

test_that("gzip output is byte-deterministic", {
  # design 18: mtime = 0, OS = 255, no name, no comment. A normal gzip
  # writes a timestamp, which would break this.
  x <- new_payload("ascii", 5000L)
  expect_identical(zu_test_stream(x, "gzip", "encode"),
                   zu_test_stream(x, "gzip", "encode"))
  z <- zu_test_stream(x, "gzip", "encode")
  expect_identical(z[5:8], as.raw(c(0, 0, 0, 0)))   # MTIME
  expect_identical(z[10], as.raw(255))              # OS unknown
  expect_identical(z[4], as.raw(0))                 # no optional fields
})

test_that("zukomp gzip output is accepted by an external decoder", {
  # memDecompress uses the zlib R links, not our vendored miniz, so this is
  # a genuine second implementation.
  x <- new_payload("structured", 8192L)
  for (lvl in c(0L, 1L, 6L, 9L)) {
    z <- zu_test_stream(x, "gzip", "encode", level = lvl)
    expect_identical(memDecompress(z, "gzip"), x, info = paste("level", lvl))
  }
})

test_that("CRC-32 and ISIZE mismatches are both checksum errors", {
  z <- zu_test_stream(new_payload("ascii", 1000L), "gzip", "encode")
  n <- length(z)

  bad_crc <- z
  bad_crc[n - 7L] <- as.raw(bitwXor(as.integer(bad_crc[n - 7L]), 0xff))
  expect_codec_error(zu_test_stream(bad_crc, "gzip", "decode"),
                     "zukomp_checksum_error")

  # ISIZE is validated against what we actually produced, never used to
  # size a buffer, so a lie costs a rejected stream and nothing else.
  bad_isize <- z
  bad_isize[n] <- as.raw(bitwXor(as.integer(bad_isize[n]), 0xff))
  expect_codec_error(zu_test_stream(bad_isize, "gzip", "decode"),
                     "zukomp_checksum_error")
})

test_that("every byte of the gzip trailer is checked", {
  z <- zu_test_stream(new_payload("ascii", 1000L), "gzip", "encode")
  n <- length(z)
  for (off in 0:7) {
    bad <- z
    bad[n - off] <- as.raw(bitwXor(as.integer(bad[n - off]), 0xff))
    expect_codec_error(zu_test_stream(bad, "gzip", "decode"),
                       "zukomp_checksum_error",
                       info = sprintf("trailer byte -%d", off))
  }
})

test_that("a corrupt FHCRC is a checksum error", {
  z <- fixture_bytes("gzip", "py_fhcrc_ascii.bin")
  # FHCRC follows the fixed 10 bytes when no other optional field is set.
  z[11] <- as.raw(bitwXor(as.integer(z[11]), 0xff))
  expect_codec_error(zu_test_stream(z, "gzip", "decode"),
                     "zukomp_checksum_error")
})

test_that("bad magic and bad compression method are rejected immediately", {
  z <- zu_test_stream(new_payload("ascii", 256L), "gzip", "encode")
  for (i in 1:3) {
    bad <- z
    bad[i] <- as.raw(bitwXor(as.integer(bad[i]), 0xff))
    expect_codec_error(zu_test_stream(bad, "gzip", "decode"),
                       "zukomp_invalid_data", info = paste("byte", i))
  }
})

test_that("reserved FLG bits are refused", {
  # RFC 1952: bits 5-7 of FLG MUST be zero. A stream that sets them is
  # malformed, not merely from a newer version we should tolerate.
  z <- zu_test_stream(new_payload("ascii", 256L), "gzip", "encode")
  for (bit in c(0x20, 0x40, 0x80)) {
    bad <- z
    bad[4] <- as.raw(bitwOr(as.integer(bad[4]), bit))
    expect_codec_error(zu_test_stream(bad, "gzip", "decode"),
                       "zukomp_invalid_data",
                       info = sprintf("FLG bit 0x%02x", bit))
  }
})

test_that("a header truncated mid-FNAME errors", {
  z <- fixture_bytes("gzip", "py_fname_ascii.bin")
  # The name is "payload.txt", starting right after the fixed 10 bytes.
  for (i in 11:18) {
    expect_codec_error(zu_test_stream(z[seq_len(i)], "gzip", "decode"),
                       "zukomp_truncated", info = sprintf("cut at %d", i))
  }
})

test_that("truncation never reports success, for gzip", {
  withr::local_seed(20260909L)
  z <- zu_test_stream(new_payload("ascii", 4096L), "gzip", "encode")
  # The header bytes are few and they are where the bugs are, so they are
  # covered exhaustively even on CRAN.
  positions <- sort(unique(c(seq_len(min(20L, length(z) - 1L)),
                             truncation_positions(length(z)))))
  for (i in positions) {
    expect_error(zu_test_stream(z[seq_len(i)], "gzip", "decode"),
                 class = "zukomp_error",
                 info = sprintf("truncated to %d of %d", i, length(z)))
  }
})

test_that("gzip streams at pathological boundaries", {
  x <- new_payload("structured", 30011L)
  for (cin in chunk_sizes()) expect_chunked_roundtrip(x, "gzip", cin, 1L)
  for (cout in chunk_sizes()) expect_chunked_roundtrip(x, "gzip", 1L, cout)
})

test_that("gzip is detectable by magic, unlike the headerless codecs", {
  d <- komp_codecs()
  expect_true(d$detectable[d$id == "gzip"])
  expect_true(d$detectable[d$id == "zlib"])          # weak predicate sniff
  expect_false(d$detectable[d$id == "deflate-raw"])  # nothing to sniff
  expect_identical(d$content_encoding[d$id == "gzip"], "gzip")
})
