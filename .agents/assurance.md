# Assurance

How this package meets `.claude/shared/assurance.md`. CI checks this
file (r-actions `assurance.yml`), so keep the headings and table
columns as they are.

- **Statuses:** a cell is a fact, `gap #<issue>` or `n/a: <reason>`.
- **Written from the code** on 2026-11-10, by reading each test's
  assertion, not the design's promises. Where a row says a test carries
  an oracle, that test compares against something that knows the answer.

## Kinds

`compiled` `untrusted-input` `spec` `lifecycle` `emits`

Evidence, kept free of code spans because this section's are read as
kinds:

- compiled: src/, with miniz, zstd and lz4 vendored.
- untrusted-input: every decoder takes compressed bytes the package did
  not produce, and libzukomp.a ships miniz's ZIP reader to consumers.
- spec: RFC 1950, 1951 and 1952, RFC 8878 (zstd), the LZ4 frame and
  block formats.
- lifecycle: the C ABI's zu_encoder and zu_decoder handles outlive the
  call that made them; consumers such as zuarrow hold them across calls.
  No R-visible object outlives a .Call today (phase 2 adds them).
- emits: every encoder writes bytes that gzip, zlib, zstd and lz4
  readers consume.

## Required checks

`Where` names the workflow file under `.github/workflows/` that runs the
check. `(own)` marks a workflow of the package's own rather than a call
to r-actions.

<!-- markdownlint-disable MD013 -->
| Check         | Status | Where                                                                                     |
| ------------- | ------ | ----------------------------------------------------------------------------------------- |
| sanitizers    | done   | native-checks.yaml                                                                        |
| valgrind      | done   | native-checks.yaml                                                                        |
| rchk          | done   | native-checks.yaml                                                                        |
| gctorture     | done   | native-checks.yaml                                                                        |
| analyzers     | done   | native-checks.yaml (GCC -fanalyzer over src/*.c; src/vendor reported, not gated)          |
| alloc-failure | done   | alloc-failure.yaml (tools/alloc/exercise.R: every codec, whole and streamed; results and sinks checked) |
| mutation      | done   | mutation.yaml (Mull over src/zu_*.c and src/codec_*.c against ctest/zkt_ctest; read, not gated) |
| fuzz          | done   | fuzz.yaml (own): libFuzzer, MSan behind a canary, ASan replay; every target has an oracle (fuzz/README.md) |
| fuzz-reach    | done   | fuzz.yaml (own), `reach` job: fuzz/reach.sh, per-file gap lists, floors in fuzz/reach-floor.tsv |
<!-- markdownlint-enable MD013 -->

## Boundaries

One row per boundary the package crosses, in each direction. An oracle
knows the answer:

- a conformance suite or test vectors;
- a reference implementation;
- a round trip;
- equivalent inputs compared;
- the bytes the other side received.

<!-- markdownlint-disable MD013 -->
| Boundary                              | Oracle                                                                                                   | Carried by                                                   |
| ------------------------------------- | -------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------ |
| deflate-raw, decode                   | Python zlib's streams decode to the known payload                                                        | tests/testthat/test-interop.R, test-acceptance.R             |
| deflate-raw, decode                   | RFC 1951 vectors built field by field, each paired with a control, with a declared outcome               | tests/testthat/test-malformed.R (fixtures/malformed/)        |
| deflate-raw, encode                   | Python zlib (wbits=-15, run to eof with no byte unused) reads zukomp's output at levels 0, 1, 6, 9 and streamed with sync flushes back to the input | tools/check-interop.sh (vendor.yaml)                         |
| zlib, decode                          | Python zlib and R `memCompress()` streams decode to the known payload, also at 1-byte chunks             | tests/testthat/test-interop.R                                |
| deflate-raw, zlib, gzip, decode       | system zlib agrees on accept or refuse and on every byte; a refusal delivers only a prefix of zlib's output; both FINISH orders agree | fuzz/fuzz_decode_raw.c, _zlib.c, _gzip.c (fuzz.yaml)          |
| zlib, encode                          | R `memDecompress()` and Python zlib read zukomp's output back to the input                               | tests/testthat/test-interop.R, tools/check-interop.sh (vendor.yaml) |
| gzip, decode                          | gzip(1) and hand-built Python members, every optional header field, decode to the known payload          | tests/testthat/test-gzip.R, test-acceptance.R                |
| gzip, encode                          | `gzip -d` and Python `gzip` read zukomp's output back to the input                                       | tools/check-interop.sh (vendor.yaml)                         |
| gzip member stream                    | an external two-member file decodes as the concatenation, at every chunk size                            | tests/testthat/test-members.R                                |
| gzip header parser                    | hand-built headers with each of FEXTRA, FNAME, FCOMMENT, FHCRC, split at every byte                      | tests/testthat/test-gzip.R                                   |
| gzip header parser                    | an independent whole-buffer reading of RFC 1952 2.3 agrees on outcome and header length               | fuzz/fuzz_gzip_header.c (fuzz.yaml)                          |
| zstd, decode                          | zstd(1) frames (multi-frame, no checksum, streamed, levels -5 to 19) decode to the known payload         | tests/testthat/test-acceptance.R, test-frames.R              |
| zstd, decode                          | libzstd's one-shot ZSTD_decompressDCtx() agrees on verdict and bytes, so the prober and stall rules are checked against code that does not stream | fuzz/fuzz_decode_zstd.c (fuzz.yaml) |
| zstd, encode                          | `zstd -d` reads zukomp's whole and streamed frames back to the input                                     | tools/check-interop.sh (vendor.yaml)                         |
| lz4-frame, decode                     | lz4(1) frames (linked, block CRC, content size, multi-frame) decode to the known payload                 | tests/testthat/test-acceptance.R, test-lz4.R                 |
| lz4-frame, decode                     | liblz4's frame decoder, given the whole input, agrees on verdict and bytes                          | fuzz/fuzz_decode_lz4_frame.c (fuzz.yaml)                     |
| lz4-frame, encode                     | `lz4 -d` reads zukomp's whole and streamed frames back to the input                                      | tools/check-interop.sh (vendor.yaml)                         |
| lz4-block, decode                     | hand-built blocks from the LZ4 block format decode to the declared bytes                                 | tests/testthat/test-lz4.R                                    |
| lz4-block, decode                     | LZ4_decompress_safe() at the exact decoded size, where liblz4 applies the end-of-block rules (#93)     | fuzz/fuzz_decode_lz4_block.c (fuzz.yaml)                     |
| lz4-block, decode                     | python-lz4 blocks (its own liblz4; default, fast, HC) decode to the known payload, told and not told the size, at every chunk size | tests/testthat/test-interop.R, test-acceptance.R             |
| lz4-block, encode                     | python-lz4's `lz4.block`, and a decoder written from the LZ4 block format that also checks its end-of-block rules, read zukomp's whole and streamed blocks back to the input | tools/check-interop.sh (vendor.yaml)                         |
| identity, both                        | round trip, output byte-identical to input                                                               | tests/testthat/test-stream.R                                 |
| every codec, encode                   | zukomp and the codec's reference (system zlib, libzstd, liblz4) decode zukomp's output back, at fuzzer-chosen levels, chunkings and flushes; the output fits zu_compress_bound() | fuzz/fuzz_roundtrip.c (fuzz.yaml) |
| every codec, chunked against whole    | encode then decode at in/out chunk sizes 1, 2, 3, 7, 31, 32, 4096 gives the input back                   | tests/testthat/helper-expect.R `expect_chunked_roundtrip()`, its callers |
| every codec, split against whole      | a decode split at fuzzer-chosen sizes, in both FINISH orders, gives the whole-buffer verdict and bytes; so do max_output and expected_size at the decoded size | fuzz/fuzz_stream_boundaries.c (fuzz.yaml) |
| every codec, chunked against whole    | the same at a `max_output` or `expected_size` of the decoded size and one either side, at every chunking | tests/testthat/test-limit-edges.R                            |
| detection (`komp_detect`, `zu_sniff`) | each codec's output, and every external fixture, is detected as its codec; headerless codecs never are   | tests/testthat/test-detect.R, test-frames.R                  |
| detection (`zu_sniff`)                | the documented magic numbers of RFC 8878, the LZ4 frame format, RFC 1952 and RFC 1950, written from the specifications | fuzz/fuzz_sniff.c (fuzz.yaml) |
| C ABI table, to a consumer            | a separate package drives the table; core limits bind on its own third-party codec                       | consumer.yaml (tools/zukomptest)                             |
| ZIP reader in `libzukomp.a`, decode   | round trip: members of a zukomp-built `probe.zip` extract to the bytes compressed                        | consumer.yaml (tools/check-linking.sh, tools/zukomplink)     |
| ZIP reader in `libzukomp.a`, decode   | Python `zipfile`'s archives list and extract to what `zipfile` reads; one-field hostile edits are read or refused as APPNOTE.TXT declares, refusals classed | tools/check-zip-reader.py over fuzz/corpus/zip/ (fuzz.yaml replay) |
| ZIP reader in `libzukomp.a`, decode   | under fuzzing: a success delivers the declared size with the declared CRC-32, the iterator, `extract_to_mem` and `extract_to_callback` agree, a refusal carries an `mz_zip_error` | fuzz/archive/fuzz_zip_reader.c (fuzz.yaml)                   |
<!-- markdownlint-enable MD013 -->

## Objects

One row per object that outlives the call that made it. "Sequences" is
the test of its out-of-order use. The guard columns name the test, or
say why the guard cannot apply.

<!-- markdownlint-disable MD013 -->
| Object                      | Sequences | Fork                                                                                         | Unwind                                                                                                     | Owner                                                                                                                       |
| --------------------------- | --------- | -------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `zu_encoder` (C ABI handle) | gap #71    | n/a: heap memory only, no descriptor, socket or thread (zstd built without ZSTD_MULTITHREAD) | the core never longjmps; zukomp's own R path holds it under a finalizer: tests/testthat/test-memory.R      | the caller, through `zu_encoder_free()` (NULL-safe); opts are copied, the vtable is static, `zu_buffer` is used only inside one call |
| `zu_decoder` (C ABI handle) | gap #71    | n/a: heap memory only, no descriptor, socket or thread                                       | the core never longjmps; zukomp's own R path holds it under a finalizer: tests/testthat/test-memory.R      | the caller, through `zu_decoder_free()` (NULL-safe); lz4 and zstd contexts run without stableSrc/stableDst, so no caller pointer is kept |
<!-- markdownlint-enable MD013 -->

## Limits

One row per limit the package enforces. "Straddled by" is the test of
`limit - 1`, `limit` and `limit + 1` on every path that reaches it.

<!-- markdownlint-disable MD013 -->
| Limit                                     | Unit                                     | Straddled by                                                                                   |
| ----------------------------------------- | ---------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `max_output`, framed codecs and identity  | decoded bytes                            | tests/testthat/test-limit-edges.R (every codec, whole and every chunking), test-frames.R, test-limits.R |
| `max_output`, DEFLATE family and lz4-block | decoded bytes                           | tests/testthat/test-limit-edges.R (every codec, whole and every chunking; nothing delivered past the limit) |
| `expected_size` (`ZU_DEC_EXPECTED_SIZE`)  | decoded bytes                            | tests/testthat/test-limit-edges.R (every codec, the one-shot ABI and every chunking)           |
| `max_ratio`                               | decoded bytes per consumed byte          | tests/testthat/test-limit-edges.R (the exact ratio whole-buffer; below it at every chunking; a mixed payload at several windows, design 22 decision 22) |
| one-shot sink capacity                    | decoded bytes                            | tests/testthat/test-limits.R, test-frames.R (`length - 1` and `length`)                        |
| zstd `windowLogMax`, derived from `max_output` | window bytes, 2^23 to 2^27          | tests/testthat/test-limit-edges.R (hand-built frames at 2^23, 2^24 and 2^27 and the next window up) |
| level range per codec                     | codec-native level                       | tests/testthat/test-limit-edges.R (`level_min - 1` to `level_max + 1` from `komp_codecs()`, R and C paths) |
| DEFLATE family miniz slice                | input bytes per miniz call, 2^31 - 1     | tests/testthat/test-limit-edges.R (lowered through `zu_test_deflate_slice()`; the real value at 2^31 - 3 to 2^31 + 2 under `ZUKOMP_HUGE_TESTS`) |
| `zu_compress_bound()` per codec           | input bytes, 0 to 2^53                   | tests/testthat/test-one-shot-compress.R (a bound at least the size and monotone, or a refusal) |
| `max_output`/`max_ratio` narrowing from R | whole numbers up to `uint64`/`uint32`    | tests/testthat/test-arg-narrowing.R                                                            |
| output sink growth (`zu_grow`)            | bytes, `SIZE_MAX`                        | tests/testthat/test-limits.R (`zu_test_grow()`: overflow refused, ordinary growth succeeds)    |
| lz4-block compressor input (`LZ4_MAX_INPUT_SIZE`) | uncompressed bytes, about 2 GiB   | tests/testthat/test-lz4.R, lowered through `zu_test_lz4b_ceilings()`: the real value needs a 2 GiB input |
| lz4-block decoder input (liblz4's `int`)  | compressed bytes, 2^31 - 1               | tests/testthat/test-lz4.R, lowered likewise; a block past the compressor's ceiling decodes (#77) |
<!-- markdownlint-enable MD013 -->

The DEFLATE `max_output` and `expected_size` rows were not only untested:
running the straddle while writing this file found #64, a valid stream
rejected when it ends exactly at the limit. test-limit-edges.R now
sweeps every input chunking for it.
