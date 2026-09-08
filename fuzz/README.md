# Fuzzing

Six libFuzzer targets over the parsers. They link the pure-C core with no R
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
| `fuzz_gzip_header` | the gzip header parser alone, byte at a time |
| `fuzz_sniff` | detection over arbitrary bytes |
| `fuzz_stream_boundaries` | decode with fuzzer-chosen chunk splits |

Two decisions worth knowing:

**Decoders run under a 16 MiB output cap.** Fuzzing a decompressor without
one just rediscovers decompression bombs and reports them as OOM. The cap
keeps findings about memory safety, which is what the fuzzer is good at.

**`fuzz_gzip_header` drives the parser directly** rather than through a full
decode, so every input is spent on the highest-risk code in the package
instead of on DEFLATE.

## Corpus

`corpus/seed/` is generated from the committed interop fixtures by
`tools/make-fuzz-corpus.sh`. `corpus/regressions/` holds minimised crashers
and is never pruned.

## When a target crashes

1. Minimise: `fuzz/build/fuzz_x -minimize_crash=1 crash-<hash>`
2. Commit the minimised input to `corpus/regressions/`.
3. **Add a testthat regression test.** A fuzz finding without one is a
   finding that can come back silently.
