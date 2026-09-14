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

# -- the member probe reads both magic bytes ----------------------------------
# A following gzip member is identified by 1f 8b, not by 1f alone. Deciding on
# the first byte committed to parsing a member too early, so any tail starting
# with 0x1f became a malformed-member error instead of a trailing-data
# decision -- and with trailing rejection switched off, data the caller had
# explicitly elected to ignore still failed the decode.

test_that("a tail starting with 0x1f is trailing data, not a bad member", {
  a <- komp_compress(charToRaw("ok"), "gzip")
  z <- c(a, as.raw(c(0x1f, 0x00)))

  expect_error(komp_decompress(z, "gzip"), class = "zukomp_trailing_bytes")
  expect_identical(
    zu_test_stream(z, "gzip", "decode", reject_trailing = FALSE),
    charToRaw("ok")
  )
})

test_that("the tail policy does not depend on the tail's first byte", {
  # 1f 00 and 99 00 are both junk; they must be treated identically.
  a <- komp_compress(charToRaw("ok"), "gzip")
  for (tail in list(c(0x1f, 0x00), c(0x99, 0x00), c(0x1f, 0x8c), c(0x1f))) {
    z <- c(a, as.raw(tail))
    expect_error(komp_decompress(z, "gzip"),
                 class = "zukomp_trailing_bytes",
                 info = paste(tail, collapse = " "))
    expect_identical(
      zu_test_stream(z, "gzip", "decode", reject_trailing = FALSE),
      charToRaw("ok"), info = paste(tail, collapse = " ")
    )
  }
})

test_that("the probe is independent of chunk boundaries", {
  # The two magic bytes can arrive in separate process() calls, and the
  # driver only refills once the codec has consumed everything -- so the
  # codec has to hold the 0x1f rather than wait on it. A lone trailing 0x1f
  # is the case that would otherwise be reported at a large chunk size and
  # silently swallowed at in_chunk = 1.
  a <- komp_compress(charToRaw("ok"), "gzip")
  for (tail in list(c(0x1f), c(0x1f, 0x00), c(0x1f, 0x8c), c(0x99, 0x00))) {
    z <- c(a, as.raw(tail))
    for (n in c(1, 2, 3, 4096)) {
      expect_error(
        zu_test_stream(z, "gzip", "decode", in_chunk = n, out_chunk = n),
        class = "zukomp_trailing_bytes",
        info = paste(paste(tail, collapse = " "), "chunk", n)
      )
      expect_identical(
        zu_test_stream(z, "gzip", "decode", in_chunk = n, out_chunk = n,
                       reject_trailing = FALSE),
        charToRaw("ok"),
        info = paste(paste(tail, collapse = " "), "chunk", n)
      )
    }
  }
})

test_that("a confirmed magic followed by a bad header is still invalid data", {
  # Once 1f 8b is confirmed the caller really did start a member, so a
  # broken header after it is a malformed member -- not trailing junk. The
  # fix must not downgrade this.
  a <- komp_compress(charToRaw("ok"), "gzip")
  z <- c(a, as.raw(c(0x1f, 0x8b, 0x00, 0x00)))
  for (n in c(1, 2, 4096)) {
    expect_error(
      zu_test_stream(z, "gzip", "decode", in_chunk = n, out_chunk = n),
      class = "zukomp_invalid_data", info = n
    )
    # and not rescued by disabling trailing rejection
    expect_error(
      zu_test_stream(z, "gzip", "decode", in_chunk = n, out_chunk = n,
                     reject_trailing = FALSE),
      class = "zukomp_invalid_data", info = n
    )
  }
})

test_that("valid concatenated members still decode at every chunk size", {
  a <- new_payload("ascii", 900)
  b <- new_payload("utf8", 1300)
  z <- c(komp_compress(a, "gzip"), komp_compress(b, "gzip"))
  expect_identical(komp_decompress(z, "gzip"), c(a, b))
  for (n in c(1, 2, 3, 17, 4096)) {
    expect_identical(
      zu_test_stream(z, "gzip", "decode", in_chunk = n, out_chunk = n),
      c(a, b), info = n
    )
  }
})

test_that("a second member split at every header byte still works", {
  a <- komp_compress(new_payload("ascii", 100), "gzip")
  b <- komp_compress(new_payload("ascii", 200), "gzip")
  z <- c(a, b)
  expect_identical(
    zu_test_stream(z, "gzip", "decode", in_chunk = 1, out_chunk = 1),
    c(new_payload("ascii", 100), new_payload("ascii", 200))
  )
})

# -- how much input the probe actually consumes --------------------------------
# "The codec stops exactly at the end of the stream and leaves src_pos exact"
# is a documented invariant, and src_pos is ABI-visible through zu_buffer, so
# a streaming consumer resuming a connection depends on it.

test_that("a rejected tail leaves the consumed count exact", {
  a <- komp_compress(charToRaw("ok"), "gzip")
  n <- length(a)
  # Tails that do not begin with 0x1f, so no probe byte is ever held. A
  # 0x1f-leading tail is the deviation pinned below.
  for (tail in list(c(0x99, 0x00), c(0x00), c(0xff, 0x8b), c(0x42))) {
    z <- c(a, as.raw(tail))
    for (chunk in c(1, 2, 3, 4096)) {
      r <- zu_test_stream(z, "gzip", "decode", in_chunk = chunk,
                          out_chunk = chunk, reject_trailing = FALSE,
                          report_consumed = TRUE)
      expect_identical(attr(r, "consumed"), as.double(n),
                       info = paste(paste(tail, collapse = " "), chunk))
    }
  }
})

test_that("valid concatenated members consume everything", {
  a <- new_payload("ascii", 500)
  b <- new_payload("utf8", 700)
  z <- c(komp_compress(a, "gzip"), komp_compress(b, "gzip"))
  for (chunk in c(1, 2, 4096)) {
    r <- zu_test_stream(z, "gzip", "decode", in_chunk = chunk,
                        out_chunk = chunk, report_consumed = TRUE)
    expect_identical(attr(r, "consumed"), as.double(length(z)), info = chunk)
    expect_identical(as.raw(r), c(a, b), info = chunk)
  }
})

test_that("a lone trailing 0x1f is consumed exactly, at every chunk size", {
  # This used to over-consume by one at *every* chunk size, not just when
  # split: ZU_FINISH only ever reached a codec with an empty buffer, so with
  # one 0x1F left the probe could not tell it was the last byte and had to
  # take it speculatively. FINISH now arrives with the final bytes.
  a <- komp_compress(charToRaw("ok"), "gzip")
  n <- length(a)
  z <- c(a, as.raw(0x1f))
  for (chunk in c(1, 2, 3, 4096)) {
    r <- zu_test_stream(z, "gzip", "decode", in_chunk = chunk,
                        out_chunk = chunk, reject_trailing = FALSE,
                        report_consumed = TRUE)
    expect_identical(attr(r, "consumed"), as.double(n), info = chunk)
    expect_identical(as.raw(r), charToRaw("ok"), info = chunk)
    expect_error(
      zu_test_stream(z, "gzip", "decode", in_chunk = chunk, out_chunk = chunk),
      class = "zukomp_trailing_bytes", info = chunk
    )
  }
})

test_that("KNOWN DEVIATION: a 1f split from its mismatch over-consumes by one", {
  # Pinned, not accepted, and now the only case left.
  #
  # When 0x1F is the last byte of a buffer and more input may follow, the
  # probe must consume it: the driver only refills once the codec has taken
  # everything, so returning ZU_NEED_INPUT on an unconsumed byte would spin.
  # If the *next* buffer then disproves the magic, that 0x1F has already been
  # counted and cannot be given back -- src_pos belongs to a call that has
  # returned. Closing it needs a pushback in the core.
  #
  # What a caller sees is unaffected: the classification is
  # zukomp_trailing_bytes at every chunk size, and the count is not reachable
  # from the R API. It is reachable by a C consumer resuming from the cursor.
  #
  # When this is fixed, this test fails and its cases move to the exact-count
  # test above.
  a <- komp_compress(charToRaw("ok"), "gzip")
  n <- length(a)

  for (tail in list(c(0x1f, 0x00), c(0x1f, 0x8c, 0x11))) {
    z <- c(a, as.raw(tail))
    lbl <- paste(sprintf("%02x", tail), collapse = " ")

    # Both magic bytes in one buffer: exact.
    for (chunk in c(3, 4096)) {
      r <- zu_test_stream(z, "gzip", "decode", in_chunk = chunk,
                          out_chunk = chunk, reject_trailing = FALSE,
                          report_consumed = TRUE)
      expect_identical(attr(r, "consumed"), as.double(n),
                       info = paste(lbl, chunk))
    }

    # Split so the 0x1F lands at a buffer boundary: one byte over.
    for (chunk in c(1, 2)) {
      r <- zu_test_stream(z, "gzip", "decode", in_chunk = chunk,
                          out_chunk = chunk, reject_trailing = FALSE,
                          report_consumed = TRUE)
      expect_identical(attr(r, "consumed"), as.double(n + 1),
                       info = paste(lbl, chunk))
    }

    # ... and none of it changes what the caller gets.
    for (chunk in c(1, 2, 3, 4096)) {
      expect_identical(
        as.raw(zu_test_stream(z, "gzip", "decode", in_chunk = chunk,
                              out_chunk = chunk, reject_trailing = FALSE)),
        charToRaw("ok"), info = paste(lbl, chunk))
      expect_error(
        zu_test_stream(z, "gzip", "decode", in_chunk = chunk,
                       out_chunk = chunk),
        class = "zukomp_trailing_bytes", info = paste(lbl, chunk))
    }
  }
})
