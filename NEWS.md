# zukomp (development version)

## New features

* `level` now accepts the abstract names `"fast"`, `"default"` and
  `"best"` wherever a codec-native integer is accepted. These are the
  portable way to express intent: numeric levels are codec-native and
  deliberately not comparable between codecs, so before this there was no
  correct level to pass from codec-agnostic code. They work on every
  codec, including ones with no level axis, where all three mean the
  codec's one behaviour.

* `komp_codecs()` gains three columns. `can_flush` reports whether a codec
  supports a mid-stream flush, which the C ABI already published through
  `zu_codec_info.flags` but R could not ask for. `level_fast` and
  `level_best` report where the abstract names land — these are declared
  by each codec rather than derived from `level_min`/`level_max`, because
  DEFLATE's level 0 is stored blocks and a codec whose level is an
  acceleration factor inverts the mapping entirely.

## Bug fixes and internals

* **Fractional decompression limits are rejected rather than truncated.**
  `max_output` and `max_ratio` are narrowed to integer types on the way to
  C, which truncates toward zero, and native `0` means "no limit" — so any
  limit in `(0, 1)` silently disabled the guard it was asked to impose.
  `komp_decompress(z, "gzip", max_output = 0.5)` decompressed a payload of
  any size. These arguments are the decompression-bomb guards and often
  arrive from options or deserialized configuration rather than from
  integer literals, so a computed fraction could quietly turn a restrictive
  policy into none. Whole numbers, `NULL`, `0` and `Inf` are unchanged.

* **A satellite codec is now usable regardless of package load order.** The
  `komp_codecs()` cache keyed on the number of rows it displayed, which is
  not a function of registry state: a satellite implementing a codec zukomp
  already *declares* — `zstd`, say — flips that row from unavailable to
  available without adding one. A table warmed before the satellite loaded
  stayed stale, so `komp_compress(codec = "zstd")` rejected the codec as not
  installed while `komp_codec_available("zstd")` returned `TRUE`. The cache
  now keys on a registry mutation counter.

* **A gzip member is identified by both magic bytes, not just `0x1f`.**
  Trailing data beginning with `0x1f` was committed to as a following
  member, so a tail of `1f 00` came back as `zukomp_invalid_data` instead of
  `zukomp_trailing_bytes` — and with trailing rejection disabled, data the
  caller had explicitly elected to ignore still failed the decode. The probe
  now confirms `1f 8b`, including when the two bytes arrive in separate
  chunks, and the outcome no longer depends on chunk boundaries.

* **`zu_register_codec()` enforces codec identity invariants.** It checked
  only that the numeric id was unused, though *name* is the key every
  R-level lookup uses — so a satellite could register the name `"gzip"` at a
  vendor id, giving `komp_codecs()` two rows with that id and making every
  R operation naming gzip fail with "the condition has length > 1" for the
  rest of the session. Registration now also requires a declared id to carry
  its declared name, an undeclared id to be in the vendor range, and names
  and content-coding tokens to be unique.

* Whole-buffer compression and decompression use much less memory. The
  output sink was on `R_alloc`, which cannot resize, so growing it meant
  allocating a new block and copying while every superseded block stayed
  live until the call returned — decoding 64 MB peaked at about 199 MB. It
  is now `malloc`/`realloc` owned by an external pointer with a finalizer,
  which keeps the interrupt-safety guarantee intact.

* miniz's assertions are compiled out explicitly, via a new
  `MINIZ_NO_ASSERT` patch. `MZ_ASSERT` expands to `assert()` at 26 sites
  reachable from malformed input, and upstream defines it unconditionally,
  so its behaviour was decided entirely by `NDEBUG`. R supplies `-DNDEBUG`,
  so ordinary and CRAN builds were already unaffected; this stops a
  `-UNDEBUG` build from aborting the R session where the shipping build
  raises a condition.

* `komp_info()$version` is a character string rather than a
  `package_version` object, so it pastes into a log line without an
  `as.character()` at every call site.

* `zu_register_codec()` now accepts a vtable compiled against an older
  header, as long as it carries every field the core dereferences. This is
  the forward-compatibility half of the `struct_size` contract; requiring
  the full current `sizeof` made every appended field a breaking change for
  satellite codecs.

## Testing

* `zu_decoder_reset()`, `zu_compress_one()` and `zu_compress_bound()` are
  now exercised, having previously had no caller outside their own
  definitions or run only against the consumer package's trivial codec.
* `ZU_FLUSH` is tested across `zlib`, `gzip` and `deflate-raw` rather than
  only against `identity`, for which a flush is trivially a no-op.

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
* Match distances are validated against the bytes emitted so far, as
  RFC 1951 requires, so a malformed stream cannot read outside the
  decompression window. This is a local patch to the vendored miniz, which
  performs the check only for a non-wrapping output buffer — a configuration
  a streaming decoder never uses. Worth knowing if you compare zukomp's
  behaviour against stock miniz: zukomp rejects three classes of stream that
  miniz accepts, all of which RFC 1951 section 3.2.5 forbids.

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
