# Changelog

## zukomp (development version)

The next release is 0.2.0: it adds codecs, which is a feature rather
than a fix.

### New codecs

- `zstd`, `lz4-frame` and `lz4-block` are built in, from vendored zstd
  1.5.7 and lz4 1.10.0
  ([\#55](https://github.com/pedrobtz/zukomp/issues/55)). They were
  planned as separate satellite packages; both libraries are small, C
  and BSD-licensed, and every consumer wants both, so they ship in the
  core instead. Their codec ids (17, 18, 19) are the ones reserved for
  them, and
  [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
  now reports all three as available.
- `codec = "auto"` and
  [`komp_detect()`](https://pedrobtz.github.io/zukomp/reference/komp_detect.md)
  recognise zstd (`28 b5 2f fd`) and LZ4 frames (`04 22 4d 18`).
  `lz4-block` has no header and, like `deflate-raw`, must be named.
- Several zstd or LZ4 frames back to back decode as one stream, as
  several gzip members do; skippable frames are stepped over, and
  anything else after the last frame is `zukomp_trailing_bytes`.
- Levels are codec-native: zstd takes -131072 to 22 (default 3, `"fast"`
  1, `"best"` 19), and both LZ4 codecs take liblz4’s scale, -65536 to 12
  (default and `"fast"` 1, `"best"` 12), where negative levels are
  acceleration.
- zstd and LZ4 frames carry a content checksum, which decoding verifies;
  a mismatch is `zukomp_checksum_error`. A frame cut short is
  `zukomp_truncated`, because unlike a DEFLATE body these formats let
  the decoder tell “needs more” from “is wrong”.
- `max_output` bounds zstd’s decoder window too: a frame header claiming
  a window larger than `max_output` allows (never less than 8 MiB) is
  refused as `zukomp_output_limit` before anything is allocated.
- `komp_info()$vendored` lists zstd and lz4, and
  `komp_info()$build_flags` their trim.

### C API

- `zu_decoder_opts` gains `expected_size`, read when `flags` has the new
  `ZU_DEC_EXPECTED_SIZE`: the exact decoded size, for a caller that
  knows it from outside the stream (an Arrow IPC body’s length prefix, a
  Parquet page header). The core holds every codec to it, like
  `max_output`: output beyond it, or a stream ending short of it, is
  `ZU_ERR_INVALID_DATA`. `lz4-block` uses it to decode in one pass;
  without it the decoder has to search for the size under `max_output`.
  The field is appended, so consumers built against 0.1.0’s header are
  unaffected and `ZUKOMP_ABI_VERSION` stays 1.

### Other changes

- [`komp_compress()`](https://pedrobtz.github.io/zukomp/reference/komp_compress.md)
  passes the whole input to the codec in one call, so zstd and LZ4
  frames record the content size and zstd sizes its tables to the input.
  Output for the DEFLATE family is unchanged.
- The licences of the bundled zstd and lz4 are installed under
  `licenses/`, beside miniz’s, and `LICENSE.note` summarises all three.

## zukomp 0.1.0

CRAN release: 2026-10-16

First release of the v1 feature set: a codec registry with a uniform
byte-in/byte-out API, one vendored codec family, and a stable C ABI.

### R API

- [`komp_compress()`](https://pedrobtz.github.io/zukomp/reference/komp_compress.md)
  and
  [`komp_decompress()`](https://pedrobtz.github.io/zukomp/reference/komp_decompress.md),
  over `identity`, `deflate-raw`, `zlib` and `gzip`. Raw vectors only —
  encoding is the caller’s decision, so character input is refused.

- [`komp_detect()`](https://pedrobtz.github.io/zukomp/reference/komp_detect.md)
  and `codec = "auto"`. Detection is a registry property, and it refuses
  to guess: headerless formats return `NA` and must be named, because
  guessing wrong there returns wrong bytes rather than an error.

- [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md),
  the capability table. It also lists the codec names zukomp reserves
  for future implementations (`zstd`, `brotli`, …) with
  `available = FALSE`; none has been released yet, and asking for one is
  a `zukomp_unsupported_codec` error that says so. Its columns include
  `can_flush`, and `level_fast`/`level_best` — where the abstract level
  names land for each codec.

- `level` accepts a codec-native whole number, `NULL` for the codec’s
  default, or one of the abstract names `"fast"`, `"default"` and
  `"best"`. Numeric levels are codec-native and deliberately not
  comparable between codecs, so the names are the portable way to say
  “compress harder”; they work on every codec, including ones with no
  level axis, where all three mean the codec’s one behaviour.

  The names are declared by each codec rather than derived from
  `level_min`/`level_max`, because the range is which integers are
  *accepted*, not which ones mean cheap and thorough: DEFLATE’s level 0
  is stored blocks, and a codec whose level is an acceleration factor
  inverts the mapping entirely.

- [`komp_codec_available()`](https://pedrobtz.github.io/zukomp/reference/komp_codec_available.md)
  and
  [`komp_info()`](https://pedrobtz.github.io/zukomp/reference/komp_info.md).
  `komp_info()$version` is a character string, so it pastes into a log
  line directly.

- Structured conditions carrying `codec`, `input_bytes`, `output_bytes`
  and `native_status`, so callers branch on the class rather than on the
  message.

### Safety

- Decompression is capped at 1 GiB by default, overridable with
  `options(zukomp.max_output = )`. An optional `max_ratio` is off by
  default, since legitimately compressible data routinely exceeds any
  safe-looking threshold.
- Both limits must be whole numbers. They are narrowed to integer types
  on the way to C, which truncates toward zero, and native `0` means *no
  limit* — so a fractional value in `(0, 1)` would otherwise disable the
  guard it was asked to impose. `NULL`, `0` and `Inf` are the spellings
  for unlimited.
- Both limits are enforced by the core stream driver rather than by
  codecs, so a third-party codec inherits them and cannot bypass them.
- Every checksum is verified, every truncation errors, and gzip’s ISIZE
  is validated against actual output — never used to size a buffer.
- Trailing data after a gzip member is a trailing-data decision, never a
  malformed-member one. A following member is confirmed on both magic
  bytes, including when they arrive in separate chunks, so the policy a
  tail receives does not depend on its first byte or on chunk size.
- miniz’s assertions are compiled out explicitly. `MZ_ASSERT` expands to
  `assert()` at sites reachable from malformed input, and upstream
  defines it unconditionally, so its behaviour was left entirely to
  `NDEBUG`; a local patch makes it overridable so that a `-UNDEBUG`
  build cannot `abort()` where an ordinary build raises a condition.
- Match distances are validated against the bytes emitted so far, as RFC
  1951 requires, so a malformed stream cannot read outside the
  decompression window. This is a local patch to the vendored miniz,
  which performs the check only for a non-wrapping output buffer — a
  configuration a streaming decoder never uses. Worth knowing if you
  compare zukomp’s behaviour against stock miniz: zukomp rejects three
  classes of stream that miniz accepts, all of which RFC 1951 section
  3.2.5 forbids.

### For package authors

- A versioned C ABI in `inst/include/zukomp.h`, reached through
  `zukomp-r.h` and R’s registered C-callable mechanism.

- Other packages can register codecs at `ZU_CODEC_VENDOR_BASE`. Adding a
  codec changes no existing declaration and needs no ABI bump.
  **Registration is experimental in this release**:
  `zu_register_codec()` and `zu_codec_vtable` are outside the ABI
  stability promise until a real satellite codec has used them, and may
  change in a minor release. The core limits apply to a registered codec
  regardless.

- `zukomp.so` exports `R_init_zukomp` and nothing else (on ELF and
  Mach-O; Windows exports through R’s `.def` file). Everything is
  reached through R’s registration tables, and an exported miniz could
  otherwise bind to another package’s vendored copy in the same process.

- `ZU_FINISH` is delivered to a codec together with the final bytes, not
  only on a later call with an empty buffer. A codec can therefore tell
  “these are the last bytes” from “here are some bytes” – which gzip’s
  next-member probe needs, so that a stream ending in a lone `0x1f` is
  consumed exactly rather than one byte over.

- Registration enforces codec identity for every registration, declared
  or not: a declared id must carry its declared name, an undeclared id
  must be in the vendor range, and names and content-coding tokens must
  be unique — including against declared codecs whose implementation is
  absent, whose names are reserved for a satellite to claim with the
  declared id. The registry is process-global and has no removal API, so
  one ambiguous registration would otherwise poison codec discovery for
  the session.

- A satellite is usable regardless of load order.
  [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
  caches on a registry mutation counter, so an implementation registered
  after the table was first read is picked up.

- `zu_codec_vtable`, `zu_codec_info`, `zu_encoder_opts` and
  `zu_decoder_opts` may all gain fields without an ABI bump: each check
  requires only the prefix the core dereferences, so a consumer compiled
  against an older header keeps working and simply does not supply the
  newer fields. Options structs are copied bounded by the caller’s own
  `struct_size`, so a shorter one is never read past its end.

- A second route, for a consumer that needs the ZIP *container* layer
  rather than a codec – the members of an `.xlsx`, for one. An installed
  zukomp ships `lib/libzukomp.a` and `include/miniz.h`, so that layer
  can be linked through `LinkingTo` instead of vendoring a second ZIP
  implementation. The archive is a separate compilation of the vendored
  `miniz.c` with the archive APIs left in; `zukomp.so` keeps exactly the
  trim it has, and contains no `mz_zip_*` code at all. The archive’s
  symbols have hidden visibility too: they link into the consumer as
  usual, but the consumer’s shared object does not re-export them. See
  “Using zukomp from C” in the README.

  Resolve it with
  `system.file("lib", .Platform$r_arch, package = "zukomp")`. It is
  architecture-specific object code, so it installs under `R_ARCH`
  beside the shared object’s directory – `lib/` on every single-arch
  platform and `lib/x64/` on Windows.

  Compile `miniz.h` with `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` defined and
  nothing else. `MINIZ_NO_TIME` in particular changes `MZ_TIME_T`, and
  with it the layout of `mz_zip_archive_file_stat`, between the
  consumer’s translation unit and the archive – a silent mismatch rather
  than a link error.

### Notes

- gzip output is deterministic — no timestamp, no filename — for a fixed
  zukomp version. It is not a content hash: compressed bytes may change
  when a vendored codec is updated.
- Whole-buffer output is built in a `realloc`’d buffer owned by an R
  external pointer, so growth releases superseded blocks as it goes and
  a large decode does not hold several at once.
- miniz 3.1.2 is vendored and trimmed; provenance is reproducible from
  `tools/vendor/manifest.tsv` and verifiable offline with
  `tools/vendor/verify`. Its MIT notice is installed at
  `licenses/miniz-LICENSE`: `miniz.h` carries no copyright line of its
  own, and an installed zukomp ships both that header and compiled miniz
  code.
- Deferred to a later release: R-level streaming objects, file and
  connection helpers, `komp_compress_text()`, dictionaries, and the
  brotli/zstd/LZ4/Snappy satellite packages.
