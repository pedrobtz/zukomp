# zukomp 0.1.0

First release of the v1 feature set: a codec registry with a uniform
byte-in/byte-out API, one vendored codec family, and a stable C ABI.

## R API

* `komp_compress()` and `komp_decompress()`, over `identity`,
  `deflate-raw`, `zlib` and `gzip`. Raw vectors only — encoding is the
  caller's decision, so character input is refused.
* `komp_detect()` and `codec = "auto"`. Detection is a registry property,
  and it refuses to guess: headerless formats return `NA` and must be
  named, because guessing wrong there returns wrong bytes rather than an
  error.
* `komp_codecs()`, the capability table, which lists codecs this build
  knows the name of even when their implementation ships elsewhere.
* `komp_codec_available()` and `komp_info()`.
* Structured conditions carrying `codec`, `input_bytes`, `output_bytes`
  and `native_status`, so callers branch on the class rather than on the
  message.

## Safety

* Decompression is capped at 1 GiB by default, overridable with
  `options(zukomp.max_output = )`. An optional `max_ratio` is off by
  default, since legitimately compressible data routinely exceeds any
  safe-looking threshold.
* Both limits are enforced by the core stream driver rather than by
  codecs, so a third-party codec inherits them and cannot bypass them.
* Every checksum is verified, every truncation errors, and gzip's ISIZE is
  validated against actual output — never used to size a buffer.

## For package authors

* A versioned C ABI in `inst/include/zukomp.h`, reached through
  `zukomp-r.h` and R's registered C-callable mechanism.
* Other packages can register codecs at `ZU_CODEC_VENDOR_BASE`. Adding a
  codec changes no existing declaration and needs no ABI bump.

## Notes

* gzip output is deterministic — no timestamp, no filename — for a fixed
  zukomp version. It is not a content hash: compressed bytes may change
  when a vendored codec is updated.
* miniz 3.1.2 is vendored and trimmed; provenance is reproducible from
  `tools/vendor/manifest.tsv` and verifiable offline with
  `tools/vendor/verify`.
* Deferred to a later release: R-level streaming objects, file and
  connection helpers, `komp_compress_text()`, dictionaries, and the
  brotli/zstd/LZ4/Snappy satellite packages.
