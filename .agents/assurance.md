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
| fuzz          | gap #68 | fuzz.yaml (own) runs libFuzzer, MSan and replay, but no harness checks more than "no crash" |
| fuzz-reach    | gap #67 | nothing measures what the corpus reaches                                                  |
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
| deflate-raw, encode                   | an external decoder reading zukomp's output                                                              | gap #69                                                       |
| zlib, decode                          | Python zlib and R `memCompress()` streams decode to the known payload, also at 1-byte chunks             | tests/testthat/test-interop.R                                |
| zlib, encode                          | R `memDecompress()` and Python zlib read zukomp's output back to the input                               | tests/testthat/test-interop.R, tools/check-interop.sh (vendor.yaml) |
| gzip, decode                          | gzip(1) and hand-built Python members, every optional header field, decode to the known payload          | tests/testthat/test-gzip.R, test-acceptance.R                |
| gzip, encode                          | `gzip -d` and Python `gzip` read zukomp's output back to the input                                       | tools/check-interop.sh (vendor.yaml)                         |
| gzip member stream                    | an external two-member file decodes as the concatenation, at every chunk size                            | tests/testthat/test-members.R                                |
| gzip header parser                    | hand-built headers with each of FEXTRA, FNAME, FCOMMENT, FHCRC, split at every byte                      | tests/testthat/test-gzip.R                                   |
| zstd, decode                          | zstd(1) frames (multi-frame, no checksum, streamed, levels -5 to 19) decode to the known payload         | tests/testthat/test-acceptance.R, test-frames.R              |
| zstd, encode                          | `zstd -d` reads zukomp's whole and streamed frames back to the input                                     | tools/check-interop.sh (vendor.yaml)                         |
| lz4-frame, decode                     | lz4(1) frames (linked, block CRC, content size, multi-frame) decode to the known payload                 | tests/testthat/test-acceptance.R, test-lz4.R                 |
| lz4-frame, encode                     | `lz4 -d` reads zukomp's whole and streamed frames back to the input                                      | tools/check-interop.sh (vendor.yaml)                         |
| lz4-block, decode                     | hand-built blocks from the LZ4 block format decode to the declared bytes                                 | tests/testthat/test-lz4.R                                    |
| lz4-block, decode                     | blocks from an external encoder                                                                          | gap #70                                                       |
| lz4-block, encode                     | an external decoder reading zukomp's output                                                              | gap #69                                                       |
| identity, both                        | round trip, output byte-identical to input                                                               | tests/testthat/test-stream.R                                 |
| every codec, chunked against whole    | encode then decode at in/out chunk sizes 1, 2, 3, 7, 31, 32, 4096 gives the input back                   | tests/testthat/helper-expect.R `expect_chunked_roundtrip()`, its callers |
| every codec, chunked against whole    | the same at a `max_output` or `expected_size` equal to the decoded size                                  | gap #65                                                       |
| detection (`komp_detect`, `zu_sniff`) | each codec's output, and every external fixture, is detected as its codec; headerless codecs never are   | tests/testthat/test-detect.R, test-frames.R                  |
| C ABI table, to a consumer            | a separate package drives the table; core limits bind on its own third-party codec                       | consumer.yaml (tools/zukomptest)                             |
| ZIP reader in `libzukomp.a`, decode   | round trip: members of a zukomp-built `probe.zip` extract to the bytes compressed                        | consumer.yaml (tools/check-linking.sh, tools/zukomplink)     |
| ZIP reader in `libzukomp.a`, decode   | archives from an external writer, and hostile ones                                                       | gap #37                                                      |
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
| `max_output`, framed codecs and identity  | decoded bytes                            | tests/testthat/test-frames.R (every chunking), test-limits.R                                   |
| `max_output`, DEFLATE family and lz4-block | decoded bytes                           | gap #65                                                                                         |
| `expected_size` (`ZU_DEC_EXPECTED_SIZE`)  | decoded bytes                            | gap #65                                                                                         |
| `max_ratio`                               | decoded bytes per consumed byte          | gap #65                                                                                         |
| one-shot sink capacity                    | decoded bytes                            | tests/testthat/test-limits.R, test-frames.R (`length - 1` and `length`)                        |
| zstd `windowLogMax`, derived from `max_output` | window bytes, 2^23 to 2^27          | gap #65                                                                                         |
| level range per codec                     | codec-native level                       | gap #65                                                                                         |
| `max_output`/`max_ratio` narrowing from R | whole numbers up to `uint64`/`uint32`    | tests/testthat/test-arg-narrowing.R                                                            |
| output sink growth (`zu_grow`)            | bytes, `SIZE_MAX`                        | tests/testthat/test-limits.R (`zu_test_grow()`: overflow refused, ordinary growth succeeds)    |
| `LZ4_MAX_INPUT_SIZE`, lz4-block           | input bytes, about 2 GiB                 | n/a: straddling it needs a 2 GiB input; the gather refuses above it (src/codec_lz4.c)          |
<!-- markdownlint-enable MD013 -->

The DEFLATE `max_output` and `expected_size` rows are not only untested:
running the straddle while writing this file found #64, a valid stream
rejected when it ends exactly at the limit.
