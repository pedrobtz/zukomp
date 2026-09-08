# Concatenated members and trailing bytes. RFC 1952 permits members to be
# concatenated and standard tools produce them, so this is correctness, not
# a nicety -- and design 17 required building it into the state machine
# rather than retrofitting it later.

test_that("concatenated gzip members concatenate payloads", {
  a <- zu_test_stream(charToRaw("hello "), "gzip", "encode")
  b <- zu_test_stream(charToRaw("world"), "gzip", "encode")
  expect_identical(zu_test_stream(c(a, b), "gzip", "decode"),
                   charToRaw("hello world"))
})

test_that("more than two members still concatenate", {
  parts <- c("one ", "two ", "three")
  members <- lapply(parts, function(p) {
    zu_test_stream(charToRaw(p), "gzip", "encode")
  })
  expect_identical(
    zu_test_stream(do.call(c, members), "gzip", "decode"),
    charToRaw(paste0(parts, collapse = ""))
  )
})

test_that("a member split across a chunk boundary still joins", {
  # The boundary that matters is the one that falls exactly on the join, so
  # it is tested directly rather than hoped for.
  a <- zu_test_stream(charToRaw("hello "), "gzip", "encode")
  b <- zu_test_stream(charToRaw("world"), "gzip", "encode")
  for (cin in c(1L, 3L, 7L, length(a) - 1L, length(a), length(a) + 1L)) {
    expect_identical(
      zu_test_stream(c(a, b), "gzip", "decode", in_chunk = cin, out_chunk = 4L),
      charToRaw("hello world"),
      info = sprintf("in_chunk = %d, member boundary at %d", cin, length(a))
    )
  }
})

test_that("the multi-member fixture from an external encoder decodes", {
  m <- fixture_manifest("gzip")
  m <- m[m$members > 1L, , drop = FALSE]
  expect_gt(nrow(m), 0L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    expect_identical(
      zu_test_stream(fixture_bytes("gzip", row$file), "gzip", "decode"),
      fixture_plaintext(row),
      info = row$file
    )
  }
})

test_that("members with different compression levels join", {
  # The external fixture concatenates a level-1 and a level-9 member, so a
  # decoder that carried per-member state across the join would fail here.
  x <- new_payload("ascii", 2048L)
  a <- zu_test_stream(x, "gzip", "encode", level = 1L)
  b <- zu_test_stream(x, "gzip", "encode", level = 9L)
  expect_identical(zu_test_stream(c(a, b), "gzip", "decode"), c(x, x))
})

test_that("junk after a zlib stream is rejected", {
  z <- c(zu_test_stream(new_payload("ascii", 1024L), "zlib", "encode"),
         as.raw(c(1, 2, 3)))
  expect_codec_error(zu_test_stream(z, "zlib", "decode"),
                     "zukomp_trailing_bytes")
})

test_that("junk after a gzip stream is trailing bytes, not a bad member", {
  # A following member must start with 1f 8b. Anything else is junk, and
  # saying so is far more useful than reporting a malformed member header.
  a <- zu_test_stream(charToRaw("hello "), "gzip", "encode")
  expect_codec_error(zu_test_stream(c(a, as.raw(1:3)), "gzip", "decode"),
                     "zukomp_trailing_bytes")
})

test_that("trailing bytes are tolerated when the caller says so", {
  # ZU_DEC_REJECT_TRAILING is policy, and policy is the caller's. With it
  # off, the decoder returns the stream's payload and stops exactly at the
  # end of the stream -- which is also the check that src_pos is exact.
  a <- zu_test_stream(charToRaw("hello "), "gzip", "encode")
  expect_identical(
    zu_test_stream(c(a, as.raw(1:3)), "gzip", "decode", reject_trailing = FALSE),
    charToRaw("hello ")
  )
  z <- zu_test_stream(new_payload("ascii", 512L), "zlib", "encode")
  expect_identical(
    zu_test_stream(c(z, as.raw(1:9)), "zlib", "decode", reject_trailing = FALSE),
    new_payload("ascii", 512L)
  )
})

test_that("member concatenation can be turned off", {
  a <- zu_test_stream(charToRaw("hello "), "gzip", "encode")
  b <- zu_test_stream(charToRaw("world"), "gzip", "encode")
  # Without ZU_DEC_CONCAT_MEMBERS the second member is simply trailing data.
  expect_codec_error(
    zu_test_stream(c(a, b), "gzip", "decode", concat_members = FALSE),
    "zukomp_trailing_bytes"
  )
  expect_identical(
    zu_test_stream(c(a, b), "gzip", "decode",
                   concat_members = FALSE, reject_trailing = FALSE),
    charToRaw("hello ")
  )
})

test_that("a truncated second member does not silently succeed", {
  # The test the roadmap singles out: it catches "we treated a partial
  # member as end-of-stream", which would silently drop data.
  a <- zu_test_stream(charToRaw("hello "), "gzip", "encode")
  b <- zu_test_stream(charToRaw("world"), "gzip", "encode")
  for (drop in 1:6) {
    expect_error(
      zu_test_stream(c(a, utils::head(b, -drop)), "gzip", "decode"),
      class = "zukomp_error",
      info = sprintf("second member short by %d bytes", drop)
    )
  }
})

test_that("a second member truncated to just its magic bytes errors", {
  a <- zu_test_stream(charToRaw("hello "), "gzip", "encode")
  expect_codec_error(
    zu_test_stream(c(a, as.raw(c(0x1f, 0x8b))), "gzip", "decode"),
    "zukomp_truncated"
  )
})

test_that("zlib never concatenates, whatever the flags", {
  # Only gzip's format permits concatenation. Two zlib streams back to back
  # are one stream plus junk.
  z <- zu_test_stream(charToRaw("hi"), "zlib", "encode")
  expect_codec_error(zu_test_stream(c(z, z), "zlib", "decode"),
                     "zukomp_trailing_bytes")
})
