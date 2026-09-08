# error messages are stable

    Code
      komp_compress(raw(1), codec = "nope")
    Condition
      Error in `komp_compress()`:
      ! Unknown codec "nope". Known codecs: identity, deflate-raw, zlib, gzip, brotli, zstd, lz4-frame, lz4-block, snappy-frame, snappy-raw.

---

    Code
      komp_compress(raw(1), codec = "zstd")
    Condition
      Error in `komp_compress()`:
      ! Codec "zstd" is known to zukomp but not installed in this build. It ships in a separate package.

---

    Code
      komp_compress(raw(1), "gzip", level = 99)
    Condition
      Error in `komp_compress()`:
      ! `level` must be between 0 and 9 for codec "gzip", not 99.

---

    Code
      komp_compress("text", "gzip")
    Condition
      Error in `komp_compress()`:
      ! `x` must be a raw vector, not character. zukomp is bytes in, bytes out; convert text with charToRaw() so the encoding is your decision.

---

    Code
      komp_decompress(as.raw(1:4), "gzip")
    Condition
      Error in `komp_decompress()`:
      ! Invalid compressed data in gzip stream.

---

    Code
      komp_decompress(komp_compress(raw(5000), "gzip"), "gzip", max_output = 16)
    Condition
      Error in `komp_decompress()`:
      ! Decompressed output exceeded `max_output`.

