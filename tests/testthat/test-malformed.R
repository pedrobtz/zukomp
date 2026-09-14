# Hand-built DEFLATE bitstreams from RFC 1951, valid and invalid, with the
# expected outcome of each recorded in fixtures/malformed/MANIFEST.tsv.
#
# This is the conformance half of the interop story. test-interop.R proves
# zukomp agrees with other encoders about *valid* streams; this file pins
# what happens on malformed ones, which is the half a network client is
# actually attacked through. test-corruption.R's bit flips reach invalid
# structures only by luck and can never say which structure they reached --
# which is why they can only assert "not silently wrong" rather than a
# class. These vectors name the structure in the filename, so the assertion
# can be exact.
#
# The manifest carries two separate judgements per vector. `rfc` is what
# RFC 1951 requires of a conforming decoder, declared from the spec and
# never observed. `expect` is what this build does. Where they disagree the
# row reads `expect = "deviation"`. There are none today -- the three that
# existed were fixed by tools/patches/miniz/0002-validate-match-distance.patch
# -- and the mechanism is kept because it is what made them visible instead
# of letting the build's behaviour quietly become the expectation.

test_that("every control vector decodes to its recorded bytes", {
  m <- malformed_manifest("ok")
  expect_gt(nrow(m), 0L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    expect_identical(
      komp_decompress(malformed_bytes(row$case), row$codec),
      hex_to_raw(row$output_hex),
      info = sprintf("%s (RFC 1951 %s)", row$case, row$rfc_section)
    )
  }
})

test_that("every control vector decodes the same one byte at a time", {
  # A hand-built stream is the harshest possible chunk-boundary test: the
  # whole stream is a handful of bytes, so every wrapper and header field
  # straddles a call boundary.
  m <- malformed_manifest("ok")
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    expect_identical(
      zu_test_stream(malformed_bytes(row$case), row$codec, "decode",
                     in_chunk = 1L, out_chunk = 1L),
      hex_to_raw(row$output_hex),
      info = row$case
    )
  }
})

test_that("every invalid vector raises its recorded condition class", {
  m <- malformed_manifest("error")
  expect_gt(nrow(m), 0L)
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    expect_codec_error(
      komp_decompress(malformed_bytes(row$case), row$codec),
      row$class,
      info = sprintf("%s (RFC 1951 %s): %s", row$case, row$rfc_section, row$note)
    )
  }
})

test_that("every invalid vector is rejected at every chunk size", {
  # Rejection must not depend on how the bytes arrive. A structural check
  # that only fires when the whole stream is in one buffer is a check zuhttp
  # would never benefit from.
  #
  # Only the parent class is asserted, not the recorded one, because *which*
  # rejection a malformed stream earns can legitimately depend on chunking.
  # raw-missing-end-of-block is the case in point: at small input chunks it
  # is stopped by the output cap rather than by a structural check, for the
  # reason recorded in its manifest note. The contract is that it is
  # rejected; the recorded class is the whole-buffer public API's answer and
  # is asserted in the test above.
  #
  # The cap is small on purpose. A stream with no end-of-block decodes
  # without bound, so an uncapped sweep here allocates until R gives up --
  # which is also the repo's standing rule that bomb tests use tiny limits
  # rather than large allocations.
  m <- malformed_manifest("error")
  for (i in seq_len(nrow(m))) {
    row <- m[i, ]
    z <- malformed_bytes(row$case)
    for (cin in chunk_sizes()) {
      expect_error(
        zu_test_stream(z, row$codec, "decode", in_chunk = cin, out_chunk = 1L,
                       max_output = 65536),
        class = "zukomp_error",
        info = sprintf("%s at in_chunk=%d", row$case, cin)
      )
    }
  }
})

test_that("a block with no end-of-block code is bounded only by the cap", {
  # Worth its own test because it is not a structural error at all: miniz
  # pads exhausted input with zero bits, and this vector's literal/length
  # table is complete with a valid one-bit code for symbol 0, so the zero
  # padding decodes to an endless run of literals. 36 bytes of input will
  # produce as much output as it is allowed to.
  #
  # That makes max_output the only thing standing between a hostile
  # 36-byte body and an unbounded allocation, which is exactly the reason
  # design 13 puts the cap in the core driver rather than in a codec.
  z <- malformed_bytes("raw-missing-end-of-block")
  expect_length(z, 36L)
  for (cap in c(1024, 8192, 65536)) {
    expect_codec_error(
      zu_test_stream(z, "deflate-raw", "decode", in_chunk = 1L, out_chunk = 1L,
                     max_output = cap),
      "zukomp_output_limit",
      info = sprintf("max_output = %d", cap)
    )
  }
})

test_that("no vector is accepted that RFC 1951 requires rejected", {
  # The strong form of the conformance contract, and the whole reason the
  # manifest records `rfc` (what the spec demands) separately from `expect`
  # (what this build does). A row where the two disagree is a deviation.
  #
  # Three RFC 1951 3.2.5 vectors were deviations until
  # tools/patches/miniz/0002-validate-match-distance.patch: tinfl validated
  # match distances only under TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF, a
  # path codec_deflate.c deliberately never takes, so a distance reaching
  # past the start of the output wrapped into miniz's malloc'd 32 KiB
  # dictionary and its contents came back as decompressed data. This test is
  # the guard: if that patch is ever dropped on a re-fetch, those rows become
  # deviations again and fail here by name.
  m <- malformed_manifest()
  expect_identical(m$case[m$expect == "deviation"], character(0))
})

test_that("a match distance beyond the bytes emitted so far is rejected", {
  # RFC 1951 3.2.5, and the security-relevant half of this corpus: for
  # `deflate-raw`, the one codec the format gives no checksum over its
  # output, accepting these meant a four-byte stream could hand the caller a
  # previous decompression's plaintext.
  #
  # Chunk sizes are swept because the check has to work on the path taken
  # when miniz's output buffer wraps -- which is every streaming call, and
  # was exactly the path that had no check at all.
  m <- malformed_manifest()
  m <- m[m$rfc_section == "3.2.5", ]
  expect_identical(nrow(m), 3L)
  for (i in seq_len(nrow(m))) {
    z <- malformed_bytes(m$case[i])
    expect_codec_error(komp_decompress(z, "deflate-raw"), "zukomp_invalid_data",
                       info = m$case[i])
    for (cin in chunk_sizes()) {
      expect_codec_error(
        zu_test_stream(z, "deflate-raw", "decode", in_chunk = cin,
                       out_chunk = 1L, max_output = 65536),
        "zukomp_invalid_data",
        info = sprintf("%s at in_chunk=%d", m$case[i], cin)
      )
    }
  }
})

test_that("the distance check does not reject matches inside a wrapped window", {
  # The counterpart to the test above, and the risk that patch carries. It
  # bounds a match distance by bytes-emitted-so-far capped at the 32 KiB
  # window, so a counter that fails to survive a call boundary, or an
  # off-by-one at the window edge, would start rejecting *valid* streams once
  # output passes 32 KiB -- a far worse regression than the bug it fixes, and
  # one no malformed vector can detect.
  #
  # Sizes straddle the window edge exactly; the one-byte sweep forces the
  # counter to be carried across a resumption for every single byte.
  for (n in c(32767L, 32768L, 32769L, 70000L)) {
    x <- new_payload("ascii", n)
    for (codec in c("deflate-raw", "zlib", "gzip")) {
      expect_roundtrip(x, codec, 9L)
    }
  }
  expect_chunked_roundtrip(new_payload("structured", 40000L), "deflate-raw", 1L, 1L)
})

test_that("a malformed body is rejected under every wrapper", {
  # The same DEFLATE body reaches all three codecs and none may return data
  # for it. This test used to be the mitigation notice -- zlib and gzip
  # caught the distance vectors on their checksums while deflate-raw returned
  # the bytes -- and now records the stronger property: the body is refused
  # before any trailer is read, so the three agree.
  #
  # The cap is needed because raw-missing-end-of-block decodes without bound;
  # it earns zukomp_output_limit rather than a structural class, so only the
  # parent class is asserted here.
  m <- malformed_manifest("error")
  for (i in seq_len(nrow(m))) {
    body <- malformed_bytes(m$case[i])
    expect_error(komp_decompress(zlib_wrap(body), "zlib", max_output = 65536),
                 class = "zukomp_error", info = m$case[i])
    expect_error(komp_decompress(gzip_wrap(body), "gzip", max_output = 65536),
                 class = "zukomp_error", info = m$case[i])
  }
})

test_that("the corpus covers every DEFLATE block encoding, with controls", {
  # A corpus of malformed streams with no valid counterparts proves only
  # that the decoder rejects things. Each block encoding needs a control, or
  # a vector that fails for an unrelated reason looks like a pass.
  m <- malformed_manifest()
  expect_true(all(m$expect %in% c("ok", "error", "deviation")))
  expect_true(all(m$rfc %in% c("ok", "reject")))
  expect_false(any(m$expect == "ok" & m$rfc == "reject"))
  expect_true(all(m$class[m$expect == "error"] != ""))

  # A control for each of the three block encodings. A corpus of rejections
  # with no valid counterparts proves only that the decoder says no to
  # something.
  for (section in c("3.2.4", "3.2.6", "3.2.7")) {
    expect_true(any(m$rfc == "ok" & m$rfc_section == section), info = section)
  }
  # And a malformation in each of the four places a DEFLATE stream can carry
  # one: the block type, a stored block's length field, the dynamic
  # code-length tables, and the distance alphabet. Fixed Huffman (3.2.6) is
  # deliberately absent -- it has no invalid form of its own, since its code
  # table is defined by the spec rather than by the stream, so its
  # malformations are distance errors filed under 3.2.5.
  for (section in c("3.2.3", "3.2.4", "3.2.5", "3.2.7")) {
    expect_true(any(m$rfc == "reject" & m$rfc_section == section), info = section)
  }
})
