test_that("detects what has magic", {
  x <- new_payload("ascii", 4096L)
  expect_identical(komp_detect(komp_compress(x, "gzip")), "gzip")
  expect_identical(komp_detect(komp_compress(x, "zlib")), "zlib")
})

test_that("detects external encoders' output too", {
  # Detection that only recognises our own bytes would be worthless to
  # zuhttp, which sees other people's streams exclusively.
  for (codec in c("gzip", "zlib")) {
    m <- fixture_manifest(codec)
    for (i in seq_len(nrow(m))) {
      expect_identical(komp_detect(fixture_bytes(codec, m$file[i])), codec,
                       info = m$file[i])
    }
  }
})

test_that("refuses to guess headerless formats", {
  z <- komp_compress(new_payload("ascii", 4096L), "deflate-raw")
  expect_identical(komp_detect(z), NA_character_)
  expect_codec_error(komp_decompress(z, "auto"), "zukomp_undetectable_codec")
})

test_that("identity is never detected", {
  # Pass-through output is indistinguishable from arbitrary bytes, so
  # detecting it would mean silently handing back the input unchanged.
  z <- komp_compress(new_payload("ascii", 4096L), "identity")
  expect_identical(komp_detect(z), NA_character_)
})

test_that("codec = auto is the default and works for detectable formats", {
  x <- new_payload("structured", 8192L)
  for (codec in c("gzip", "zlib")) {
    expect_identical(komp_decompress(komp_compress(x, codec)), x, info = codec)
  }
})

test_that("magic beats a predicate sniffer", {
  # A gzip stream's first two bytes are 1f 8b, which must never be judged by
  # zlib's weak header predicate first.
  z <- komp_compress(new_payload("ascii", 1024L), "gzip")
  expect_identical(komp_detect(z), "gzip")
})

test_that("too-short input is undetectable rather than an error", {
  for (n in 0:1) {
    expect_identical(komp_detect(raw(n)), NA_character_, info = paste("n =", n))
  }
  expect_codec_error(komp_decompress(raw(0), "auto"),
                     "zukomp_undetectable_codec")
})

test_that("random bytes are not mistaken for zlib", {
  # The honest test: it documents the false-positive rate rather than
  # pretending detection is exact. zlib's header is a predicate -- CM == 8,
  # CINFO <= 7, and the two bytes a multiple of 31 -- which about one
  # arbitrary byte pair in a thousand satisfies.
  withr::local_seed(11L)
  hits <- vapply(1:2000, function(i) {
    komp_detect(as.raw(sample.int(256L, 8L, replace = TRUE) - 1L))
  }, character(1))
  expect_lt(mean(!is.na(hits)), 0.01)
  # ...and whatever it does match must be zlib, never a magic-bearing codec
  expect_true(all(hits[!is.na(hits)] == "zlib"))
})

test_that("detection does not consume or alter its input", {
  z <- komp_compress(new_payload("ascii", 512L), "gzip")
  before <- z
  komp_detect(z)
  expect_identical(z, before)
  expect_identical(komp_decompress(z, "gzip"), new_payload("ascii", 512L))
})

test_that("komp_codecs() agrees with what detection can actually do", {
  # The `detectable` column is a promise; this is the test that it is kept.
  d <- komp_codecs()
  x <- new_payload("ascii", 1024L)
  for (codec in d$id[d$available & d$can_encode]) {
    detected <- komp_detect(komp_compress(x, codec))
    expect_identical(!is.na(detected), d$detectable[d$id == codec],
                     info = codec)
  }
})

test_that("a zlib stream needing a preset dictionary is unsupported, not undetected", {
  # It really is zlib; we just cannot decode it. Saying so is more useful
  # than claiming not to recognise it.
  z <- komp_compress(new_payload("ascii", 512L), "zlib")
  hdr <- as.integer(z[1:2])
  flg <- bitwOr(bitwAnd(hdr[2], 0xE0), 0x20)
  base <- bitwShiftL(hdr[1], 8) + flg
  z[2] <- as.raw(flg + (31 - (base %% 31)) %% 31)
  expect_identical(komp_detect(z), "zlib")
  expect_codec_error(komp_decompress(z, "auto"), "zukomp_unsupported_codec")
})
