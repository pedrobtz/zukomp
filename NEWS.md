# zukomp (development version)

The next release is 0.2.0: it adds codecs, which is a feature rather than a
fix.

## The ZIP reader in `libzukomp.a`

* For packages linking the static library (`zuxlsx`): miniz's ZIP reader
  now refuses, with an `mz_zip_error`, four hostile archives it used to
  mishandle (#92). Reading a deflated member from an archive in memory
  through the extraction iterator no longer hangs when the member's DEFLATE
  stream is cut short. `mz_zip_reader_extract_to_mem()` no longer reads past
  a stored entry whose two sizes differ. A corrupt DEFLATE stream now sets
  `MZ_ZIP_DECOMPRESSION_FAILED` instead of leaving the error at
  `MZ_ZIP_NO_ERROR`. An entry with no compressed data is checked as
  the iterator checks it (local header, method, size, CRC-32) instead of
  being returned as empty. And an archive whose central directory sits more than 4 KiB
  from its end, behind a long archive comment, now opens instead of being
  refused. Found by a new fuzz target and a comparison with Python's
  `zipfile` (#37). A consumer gets the fix when it is reinstalled.

## New codecs

* `zstd`, `lz4-frame` and `lz4-block` are built in, from vendored zstd 1.5.7
  and lz4 1.10.0 (#55). They were planned as separate satellite packages;
  both libraries are small, C and BSD-licensed, and every consumer wants
  both, so they ship in the core instead. Their codec ids (17, 18, 19) are
  the ones reserved for them, and `komp_codecs()` now reports all three as
  available.
* `codec = "auto"` and `komp_detect()` recognise zstd (`28 b5 2f fd`) and
  LZ4 frames (`04 22 4d 18`). `lz4-block` has no header and, like
  `deflate-raw`, must be named.
* Several zstd or LZ4 frames back to back decode as one stream, as several
  gzip members do; skippable frames are stepped over, and anything else
  after the last frame is `zukomp_trailing_bytes`.
* Levels are codec-native: zstd takes -131072 to 22 (default 3, `"fast"` 1,
  `"best"` 19), and both LZ4 codecs take liblz4's scale, -65536 to 12
  (default and `"fast"` 1, `"best"` 12), where negative levels are
  acceleration.
* zstd and LZ4 frames carry a content checksum, which decoding verifies; a
  mismatch is `zukomp_checksum_error`. A frame cut short is
  `zukomp_truncated`, because unlike a DEFLATE body these formats let the
  decoder tell "needs more" from "is wrong".
* `max_output` bounds zstd's decoder window too: a frame header claiming a
  window larger than `max_output` allows (never less than 8 MiB), and no
  content size within it, is refused as `zukomp_output_limit` before
  anything is allocated.
* `komp_info()$vendored` lists zstd and lz4, and `komp_info()$build_flags`
  their trim.

## C API

* `zukomp-r.h`'s resolver refuses an API table smaller than the one the
  consumer was compiled against: a zukomp older than the consumer's header,
  without entries appended since. Appends do not change
  `ZUKOMP_ABI_VERSION`, which marks a break only, so the version check
  alone could not catch it. `zukomp-r.h` had described the version as the
  way fields are appended; its comments now say what the code does.

* `zu_decoder_opts` gains `expected_size`, read when `flags` has the new
  `ZU_DEC_EXPECTED_SIZE`: the exact decoded size, for a caller that knows it
  from outside the stream (an Arrow IPC body's length prefix, a Parquet page
  header). The core holds every codec to it, like `max_output`: output
  beyond it, or a stream ending short of it, is `ZU_ERR_INVALID_DATA`.
  `lz4-block` uses it to decode in one pass; without it the decoder has to
  search for the size under `max_output`. The field is appended, so
  consumers built against 0.1.0's header are unaffected and
  `ZUKOMP_ABI_VERSION` stays 1.
* Encoder and decoder handles follow one specified lifecycle, enforced by
  the core for every codec (#71). This changes behaviour that was
  unspecified, for C consumers only; no function, type or enum value
  changed.
  * A stream's first error is sticky: every later `zu_*_process()` returns
    the same status and moves neither cursor until a reset. Before, the
    DEFLATE codecs repeated their error, zstd and lz4-frame returned a
    different one per call and then decoded fresh input, and after an
    expected-size, trailing-data or output-limit failure every codec
    answered `ZU_STREAM_END` on the next call (#83).
  * `ZU_RUN` or `ZU_FLUSH` after a call that passed `ZU_FINISH` is refused
    with `ZU_ERR_INVALID_ARGUMENT`, and the stream carries on. Before, the
    codecs disagreed: the DEFLATE encoders failed with invalid data, LZ4
    ended the frame and dropped the new input, and zstd took it.
  * Input to a decoder after `ZU_STREAM_END` is `ZU_ERR_TRAILING`, and a
    failure, when `ZU_DEC_REJECT_TRAILING` is set, whichever call it
    arrives in.
  * A call refused before the stream sees it (a bad buffer, `ZU_FLUSH` on
    a codec that cannot flush) changes nothing, and a reset the codec fails
    leaves the handle failed.
* `src_pos` is documented as exact at the end of a stream, with one
  exception: under `ZU_DEC_CONCAT_MEMBERS`, when the next member's or
  frame's magic straddles a buffer boundary, up to one byte (gzip) or three
  (zstd, lz4-frame) may be counted that turn out not to be a magic (#79).
  Decoding without the flag gives the exact end.

## Bug fixes

* The drive loop behind `komp_compress()` and `komp_decompress()` created
  its stream handle before the external pointer that owns it, so a longjmp
  while allocating that pointer leaked the handle. The owner is created
  first now (#78).

* A zstd decoder handle that was called 16 times in a row without moving a
  byte (waiting for input, or for output room) reported its valid stream
  as `ZU_ERR_INVALID_DATA`. That was libzstd's guard against a spinning
  caller, which is not an error in the stream; such calls now answer
  `ZU_NEED_INPUT` or `ZU_NEED_OUTPUT` however many there are (#100).

* `deflate-raw`, `zlib` and `gzip` refuse the codes RFC 1951 reserves:
  the literal/length symbols 286 and 287 and the distance symbols 30 and
  31, which a fixed-Huffman block can still carry, and a dynamic block
  header that declares more than 286 literal/length or 30 distance codes.
  miniz decoded a reserved symbol as an empty match or a distance of 0, so
  a malformed `deflate-raw` stream came back as data, and `zlib` and `gzip`
  delivered bytes before failing on the checksum (#94). The fix is a new
  local miniz patch, `0006-reject-reserved-codes.patch`.

* A truncated `deflate-raw`, `zlib` or `gzip` stream delivers the same
  bytes before its error whichever call carries `ZU_FINISH`. When it came
  with the final bytes, miniz stopped short of output it had already
  decoded (#68).

* `lz4-block` gives a block one verdict whatever the limits. If the block
  breaks the format's end-of-block rules (the last 5 bytes are literals,
  and the last match starts at least 12 bytes from the end), it is now
  always `zukomp_invalid_data`. The size search used to accept such a
  block, or call it `zukomp_output_limit` when `max_output` was its size
  (#93).

* A zstd frame that declares its content size and ends in an empty last
  block is held to that size. libzstd's streaming decoder skipped the
  check there, so such a frame decoded as empty when it arrived in chunks
  smaller than its declared size, and was refused otherwise (#68). The fix
  is a local zstd patch, `0002-check-content-size-after-empty-last-block.patch`.

* A valid zstd frame with a zero-byte window -- single segment, declared
  content size 0 -- ending in an RLE block of size 0 decodes at every chunk
  size. libzstd's streaming decoder compared the RLE block's one-byte body,
  not its size, with the frame's maximum block size of 0, and called the
  frame corrupt whenever it arrived a few bytes at a time (#68). The fix is
  a local zstd patch, `0003-check-rle-block-size-not-body.patch`.

* A zstd compressed block of size 0 is `zukomp_invalid_data` at every chunk
  size. libzstd's streaming decoder took it for an empty block, so a frame
  ending in one was a checksum failure, or accepted, when it arrived in
  small chunks (#68). The fix is a local zstd patch,
  `0004-refuse-empty-compressed-block.patch`.

* A stream that exceeds `max_ratio` is `zukomp_ratio_limit` whatever else
  is wrong with it. When one output window held the whole stream, trailing
  bytes, a `ZU_DEC_EXPECTED_SIZE` the stream fell short of or ran past, or
  `max_output` were reported instead, while the same bytes in smaller
  windows reported the ratio.
* A valid zstd frame that declares a large window and a small content size
  decodes at every output chunk size (#119). libzstd's buffer for such a
  frame is the content size, but its streaming decoder refused it on the
  window alone, unless the first call's output room happened to cover the
  content -- so the same frame decoded whole and was refused as
  `zukomp_output_limit` (or unsupported, with no limit) when read in small
  pieces. zukomp now reads each frame header before libzstd does and
  refuses a frame only when the smaller of its window and its declared
  content size is above the ceiling. Such a frame cut short is now
  `zukomp_truncated`, where it was `zukomp_output_limit`.

* `komp_compress()` and `komp_decompress()` fail with
  `zukomp_internal_error` when a codec stops making progress, instead of
  looping until interrupted. Only one stuck shape was caught before; a
  registered codec that answered `ZU_OK` without moving a byte, or asked
  for input while leaving its input unread, hung the session. No built-in
  codec does either.
* A zstd frame whose window is larger than this build supports is
  `zukomp_unsupported_codec` under any `max_output` above 2^26 bytes, as
  it is with no limit. It was `zukomp_output_limit`, although no limit
  could have admitted it. `zukomp_output_limit` now means the window
  exceeded the ceiling `max_output` sets; under a limit of 2^26 or less
  that is still the answer for any oversized window, since libzstd does
  not report the window's size.
* zstd and lz4-frame report the same frame problems the same way. A frame
  that names a dictionary is `zukomp_unsupported_codec` for both: lz4-frame
  decoded it without the dictionary and failed as `zukomp_invalid_data`, or
  succeeded when no block used it. A reserved header bit that is set is
  `zukomp_invalid_data` for both: zstd reported it as unsupported.
* A refused `komp_decompress()` or `komp_compress()` no longer copies its
  partial output into R just to report its length. A decompression bomb
  stopped at `max_output` therefore costs the cap, not twice it -- 1 GiB,
  not 2, at the default -- and a host that could not spare the second copy
  no longer gets R's "cannot allocate vector" in place of
  `zukomp_output_limit`. The condition's `output_bytes` keeps its value,
  and is now a double, as its documented `NA_real_` default always was.
* A streaming decoder no longer answers `ZU_NEED_INPUT` while it still
  holds decoded output. Having taken all the input it was offered, a
  `deflate-raw`, `zlib`, `gzip`, `zstd` or `lz4-frame` decoder whose output
  window had filled said it needed input -- with up to 32 KiB still owed
  for the DEFLATE family -- so a C caller that waited for more input, as
  the status says to, stalled until more arrived or it sent `ZU_FINISH`.
  It now asks its library for one byte first and answers `ZU_NEED_OUTPUT`
  when there is one. `komp_decompress()` was not affected.

  One verdict changes with it: a `deflate-raw`, `zlib` or `gzip` body cut
  short exactly at `max_output` is `zukomp_invalid_data`, the class it has
  with no limit, where it was `zukomp_output_limit`. Nothing was left for
  the limit to refuse.

* The `deflate-raw`, `zlib` and `gzip` decoders no longer reject a valid
  stream that ends exactly at `max_output` (as `zukomp_output_limit`) or at
  `ZU_DEC_EXPECTED_SIZE` (as `zukomp_invalid_data`) when an input chunk ends
  after the last output byte but before the end-of-block code or the
  trailer (#64). `komp_decompress()` passes its input in 64 KiB slices, so
  it could meet this only with a longer stream; streaming consumers of the
  C API met it at ordinary chunk sizes.
* A DEFLATE block whose code table gives the end-of-block symbol no code is
  now `zukomp_invalid_data` before any output, however its bytes arrive
  (#74). miniz decoded such a block's literals instead, so at small input
  chunks it produced output until `max_output` stopped it with
  `zukomp_output_limit`, and without a cap it never stopped. A fourth local
  miniz patch, `0004-reject-missing-end-of-block.patch`, adds the check zlib
  makes.

* The `lz4-block` decoder no longer applies the compressor's input ceiling
  (`LZ4_MAX_INPUT_SIZE`, about 2 GiB of uncompressed bytes) to a compressed
  block, which for incompressible data is larger (#77). It takes blocks up
  to liblz4's own limit, 2^31 - 1 bytes; a larger one is
  `zukomp_unsupported_codec`.

* `deflate-raw`, `zlib` and `gzip` no longer silently truncate an input
  longer than 2^31 - 1 bytes passed in one call, which every
  `komp_compress()` is (#73). miniz was told the first 2^31 - 1 bytes were
  the whole input, and wrote a valid stream of that prefix with a matching
  checksum, so nothing downstream could tell. An encoder that ends its
  stream with input unread is now an internal error, for every codec.
* `zu_compress_bound()` for `lz4-frame` no longer returns a tiny bound for
  sizes from 2^48 bytes, where liblz4's block count wrapped; those sizes are
  refused (#85).

* `komp_decompress()` rejects bytes after the stream as
  `zukomp_trailing_bytes` wherever the stream ends. A stream that ended on
  a 64 KiB input boundary was accepted with whatever followed it dropped
  (#90).
* `ZU_FLUSH` on a `deflate-raw`, `zlib` or `gzip` encoder returns once the
  bytes so far are out. It used to fill all the output room it was given
  with empty stored blocks, so a C caller flushing until the encoder
  stopped asking for room never finished (#91).
* On a 32-bit platform, a DEFLATE stream decoding to more than 4 GiB
  through one decoder no longer fails part way with `zukomp_invalid_data`:
  the history count in miniz patch 0002 saturates at the window instead of
  wrapping (#86).

## Other changes

* `max_ratio` is documented as what it has always been: a bound on expansion
  as decoding proceeds, checked against the compressed bytes read so far,
  not only on the final ratio (#80). A stream whose start is far more
  compressible than the rest can be refused although its overall ratio is
  under the limit.

* `komp_compress()` passes the whole input to the codec in one call, so zstd
  and LZ4 frames record the content size and zstd sizes its tables to the
  input. Output for the DEFLATE family is unchanged.
* The licences of the bundled zstd and lz4 are installed under
  `licenses/`, beside miniz's, and `LICENSE.note` summarises all three.


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
