# Fuzzing

Ten libFuzzer targets over the parsers. They link the pure-C core with no R
in the process, which is only possible while `src/zu_internal.h` stays free
of R — an `Rinternals.h` error when building here means something leaked
into the core.

```sh
fuzz/build.sh          # needs clang with libFuzzer
fuzz/run.sh 60         # 60 seconds per target
```

| target | what it exercises |
|---|---|
| `fuzz_decode_gzip` / `_zlib` / `_raw` | whole-stream decode per codec |
| `fuzz_decode_zstd` / `_lz4_frame` / `_lz4_block` | the same, for the codecs added in #55: zukomp's driving of libzstd and liblz4 (stall classification, frame continuation, lz4-block's size search), not the libraries themselves, which are fuzzed upstream |
| `fuzz_gzip_header` | the gzip header parser alone, byte at a time |
| `fuzz_sniff` | detection over arbitrary bytes |
| `fuzz_stream_boundaries` | decode with fuzzer-chosen chunk splits |
| `fuzz_zip_reader` | the ZIP reader `libzukomp.a` ships (#37), built as the archive builds it: open from memory, stat, locate, extract through the iterator, into memory and through a write callback, with oracles (below) |

Two decisions worth knowing:

**Decoders run under a 16 MiB output cap.** Fuzzing a decompressor without
one just rediscovers decompression bombs and reports them as OOM. The cap
keeps findings about memory safety, which is what the fuzzer is good at.

**`fuzz_gzip_header` drives the parser directly** rather than through a full
decode, so every input is spent on the highest-risk code in the package
instead of on DEFLATE.

**`fuzz_zip_reader` has oracles, not only sanitizers.** It aborts when a
refusal carries no `mz_zip_error`, when a success delivers other than the
entry's declared size or CRC-32 (computed with its own table, not miniz's),
or when the iterator, `mz_zip_reader_extract_to_mem()` and
`mz_zip_reader_extract_to_callback()` disagree. Its
source is in `archive/`, outside `build.sh`'s `fuzz_*.c` loop, because it
compiles `miniz.c` alone with the archive's defines rather than with the
core. With `ZU_FUZZ_ZIP_DUMP` set it prints what it read, which
`tools/check-zip-reader.py` compares with Python's `zipfile`.

## Corpus

`corpus/seed/` is generated from the committed interop fixtures by
`tools/make-fuzz-corpus.sh`. `corpus/regressions/` holds minimised crashers
and is never pruned. `corpus/zip/` is committed: ZIP seeds from
`tools/make-zip-corpus.py` (`--check` regenerates and diffs), with a
`MANIFEST.tsv` that declares from APPNOTE.TXT whether each must be read or
refused.

## When a target crashes

1. Minimise: `fuzz/build/fuzz_x -minimize_crash=1 crash-<hash>`
2. Commit the minimised input to `corpus/regressions/`.
3. **Add a testthat regression test.** A fuzz finding without one is a
   finding that can come back silently.
