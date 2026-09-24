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
* `komp_codecs()`, the capability table. It also lists the codec names
  zukomp reserves for future implementations (`zstd`, `brotli`, ...) with
  `available = FALSE`; none has been released yet, and asking for one is a
  `zukomp_unsupported_codec` error that says so. Its
  columns include `can_flush`, and `level_fast`/`level_best` — where the
  abstract level names land for each codec.
* `level` accepts a codec-native whole number, `NULL` for the codec's
  default, or one of the abstract names `"fast"`, `"default"` and
  `"best"`. Numeric levels are codec-native and deliberately not
  comparable between codecs, so the names are the portable way to say
  "compress harder"; they work on every codec, including ones with no
  level axis, where all three mean the codec's one behaviour.

  The names are declared by each codec rather than derived from
  `level_min`/`level_max`, because the range is which integers are
  *accepted*, not which ones mean cheap and thorough: DEFLATE's level 0 is
  stored blocks, and a codec whose level is an acceleration factor inverts
  the mapping entirely.
* `komp_codec_available()` and `komp_info()`. `komp_info()$version` is a
  character string, so it pastes into a log line directly.
* Structured conditions carrying `codec`, `input_bytes`, `output_bytes`
  and `native_status`, so callers branch on the class rather than on the
  message.

## Safety

* Decompression is capped at 1 GiB by default, overridable with
  `options(zukomp.max_output = )`. An optional `max_ratio` is off by
  default, since legitimately compressible data routinely exceeds any
  safe-looking threshold.
* Both limits must be whole numbers. They are narrowed to integer types on
  the way to C, which truncates toward zero, and native `0` means *no
  limit* — so a fractional value in `(0, 1)` would otherwise disable the
  guard it was asked to impose. `NULL`, `0` and `Inf` are the spellings
  for unlimited.
* Both limits are enforced by the core stream driver rather than by
  codecs, so a third-party codec inherits them and cannot bypass them.
* Every checksum is verified, every truncation errors, and gzip's ISIZE is
  validated against actual output — never used to size a buffer.
* Trailing data after a gzip member is a trailing-data decision, never a
  malformed-member one. A following member is confirmed on both magic
  bytes, including when they arrive in separate chunks, so the policy a
  tail receives does not depend on its first byte or on chunk size.
* miniz's assertions are compiled out explicitly. `MZ_ASSERT` expands to
  `assert()` at sites reachable from malformed input, and upstream defines
  it unconditionally, so its behaviour was left entirely to `NDEBUG`; a
  local patch makes it overridable so that a `-UNDEBUG` build cannot
  `abort()` where an ordinary build raises a condition.
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
  **Registration is experimental in this release**: `zu_register_codec()`
  and `zu_codec_vtable` are outside the ABI stability promise until a real
  satellite codec has used them, and may change in a minor release. The
  core limits apply to a registered codec regardless.
* `zukomp.so` exports `R_init_zukomp` and nothing else (on ELF and
  Mach-O; Windows exports through R's `.def` file). Everything is reached
  through R's registration tables, and an exported miniz could otherwise
  bind to another package's vendored copy in the same process.
* `ZU_FINISH` is delivered to a codec together with the final bytes, not
  only on a later call with an empty buffer. A codec can therefore tell
  "these are the last bytes" from "here are some bytes" -- which gzip's
  next-member probe needs, so that a stream ending in a lone `0x1f` is
  consumed exactly rather than one byte over.

* Registration enforces codec identity for every registration, declared
  or not: a declared id must carry its declared name, an undeclared id
  must be in the vendor range, and names and content-coding tokens must
  be unique — including against declared
  codecs whose implementation is absent, whose names are reserved for a
  satellite to claim with the declared id. The registry is process-global
  and has no removal API, so one ambiguous registration would otherwise
  poison codec discovery for the session.
* A satellite is usable regardless of load order. `komp_codecs()` caches on
  a registry mutation counter, so an implementation registered after the
  table was first read is picked up.
* `zu_codec_vtable`, `zu_codec_info`, `zu_encoder_opts` and
  `zu_decoder_opts` may all gain fields without an ABI bump: each check
  requires only the prefix the core dereferences, so a consumer compiled
  against an older header keeps working and simply does not supply the
  newer fields. Options structs are copied bounded by the caller's own
  `struct_size`, so a shorter one is never read past its end.
* A second route, for a consumer that needs the ZIP *container* layer
  rather than a codec -- the members of an `.xlsx`, for one. An installed
  zukomp ships `lib/libzukomp.a` and `include/miniz.h`, so that layer can
  be linked through `LinkingTo` instead of vendoring a second ZIP
  implementation. The archive is a separate compilation of the vendored
  `miniz.c` with the archive APIs left in; `zukomp.so` keeps exactly the
  trim it has, and contains no `mz_zip_*` code at all. The archive's
  symbols have hidden visibility too: they link into the consumer as usual,
  but the consumer's shared object does not re-export them. See "Using
  zukomp from C" in the README.

  Resolve it with `system.file("lib", .Platform$r_arch, package =
  "zukomp")`. It is architecture-specific object code, so it installs
  under `R_ARCH` beside the shared object's directory -- `lib/` on every
  single-arch platform and `lib/x64/` on Windows.

  Compile `miniz.h` with `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` defined and
  nothing else. `MINIZ_NO_TIME` in particular changes `MZ_TIME_T`, and
  with it the layout of `mz_zip_archive_file_stat`, between the consumer's
  translation unit and the archive -- a silent mismatch rather than a link
  error.

## Notes

* gzip output is deterministic — no timestamp, no filename — for a fixed
  zukomp version. It is not a content hash: compressed bytes may change
  when a vendored codec is updated.
* Whole-buffer output is built in a `realloc`'d buffer owned by an R
  external pointer, so growth releases superseded blocks as it goes and a
  large decode does not hold several at once.
* miniz 3.1.2 is vendored and trimmed; provenance is reproducible from
  `tools/vendor/manifest.tsv` and verifiable offline with
  `tools/vendor/verify`. Its MIT notice is installed at
  `licenses/miniz-LICENSE`: `miniz.h` carries no copyright line of its own,
  and an installed zukomp ships both that header and compiled miniz code.
* Deferred to a later release: R-level streaming objects, file and
  connection helpers, `komp_compress_text()`, dictionaries, and the
  brotli/zstd/LZ4/Snappy satellite packages.
