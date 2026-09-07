# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`zukomp` is an R package: a **codec registry with a uniform byte-in/byte-out compression API**, backed by vendored C sources (miniz first) and no system libraries. It exists to serve `zuhttp` and future `zu*` packages through a stable, registered C-callable ABI.

The framing that governs every design decision: **zukomp is a codec registry that ships with DEFLATE, not a DEFLATE package with room for extras.** When a choice would make DEFLATE special, it is the wrong choice.

## Current state

**All 15 stages are complete; the package is at v1 (version 0.1.0).** miniz 3.1.2 is vendored under `src/vendor/miniz/`, four codecs are registered — `identity`, `deflate-raw`, `zlib`, `gzip` — and the public R API is `komp_compress()`, `komp_decompress()`, `komp_detect()`, `komp_codecs()`, `komp_codec_available()`, `komp_info()`. `devtools::check(cran = TRUE)` is 0/0/0; 1097 tests pass, 1959 with `ZUKOMP_SLOW_TESTS=true`.

**One acceptance criterion is open, deliberately.** Design §24 criterion 11 names `zuhttp`, which is still an empty skeleton in its own repo. Stage 15 was therefore done as an integration spike inside `tests/consumer/zukomptest`, covering all four of design §16's contract points; the criterion cannot be closed until a real `zuhttp` exists. Both design docs record this.

**Phase 2 is what comes next**, not more of Stage 15: R-level streaming objects, file and connection helpers, `komp_compress_text()`, a benchmark vignette, then the brotli/zstd/LZ4/Snappy satellites. `src/zu_miniz.c` is still Stage 1 scaffolding behind `zukomp:::zu_miniz_version()`, now redundant with `komp_info()` and removable.

Functions were added to the header by the stage that implemented them, so it never advertises a symbol that will not link — keep that rule for phase 2. Everything described below is shipped code.

## The design docs are the spec

[.agents/design-zukomp.md](.agents/design-zukomp.md) and [.agents/ROADMAP.md](.agents/ROADMAP.md) are not background reading — they are the binding specification, down to struct field order, enum values, symbol prefixes, and per-stage test code. **Read the relevant section before writing code**, and if implementation reveals the design is wrong, change the design doc in the same commit rather than diverging from it silently.

- `design-zukomp.md` — numbered sections §1–§25 (§7 error model, §8–§10 C ABI and vtable, §13 memory/longjmp rules, §15 downstream linkage, §22 decision log resolving all open questions).
- `ROADMAP.md` — Stages 0–15 to v1, each with an explicit **Verify** block. A stage is not done until its verification block runs clean *and* earlier stages still pass.

## Commands

```sh
Rscript -e 'devtools::document()'                     # roxygen -> NAMESPACE + man/
Rscript -e 'devtools::load_all()'                     # compile + load for interactive work
Rscript -e 'devtools::test()'
Rscript -e 'devtools::test(shuffle = TRUE)'           # required before calling a stage done
Rscript -e 'devtools::check()'                        # target: 0 errors, 0 warnings, 0 notes
```

Offline, `check()` emits a spurious `checking for future file timestamps ... NOTE / unable to verify current time` because the clock check cannot reach the network. Suppress it to see the real result:

```sh
Rscript -e 'devtools::check(env_vars = c("_R_CHECK_SYSTEM_CLOCK_" = "0"))'
```

Single test file / subset:

```sh
Rscript -e 'devtools::test(filter = "gzip")'          # runs tests/testthat/test-gzip.R
Rscript -e 'devtools::load_all(); testthat::test_file("tests/testthat/test-gzip.R")'
```

Exhaustive sweeps and large-buffer tests are gated behind `skip_if_no_slow_tests()`:

```sh
ZUKOMP_SLOW_TESTS=true Rscript -e 'devtools::test()'
```

### Vendored sources

Third-party code under `src/vendor/` is **never edited in place**. The whole tree is reproducible from `tools/vendor/manifest.tsv`:

```sh
./tools/vendor/verify            # offline; run this after touching src/vendor/ or src/Makevars
./tools/vendor/fetch [source]    # maintainer-only, needs network: download, checksum, trim, patch
./tools/vendor/record            # regenerate checksums.sha256 after a deliberate fetch
```

`verify` fails on a modified vendored file, an unrecorded new one, and any drift between `src/Makevars`'s `-D` flags and the manifest's `defines` column — in *both* directions. It also cross-checks miniz's `MZ_VERSION` against the manifest and against the literal asserted in `test-abi.R` (tests cannot read the manifest, because `tools/` is not installed). CI additionally rejects a PR that changes `src/vendor/` without updating both the manifest and `checksums.sha256`.

To change a vendored source: edit `manifest.tsv`, run `fetch`, review the `src/vendor/` diff, commit both together. Local modifications belong in `tools/patches/<source>/` as patch files that `fetch` applies — ideally none.

Two things about the miniz trim that are easy to get wrong:

- The define set is **six**, not the five design §12 originally listed. `MINIZ_NO_PNG_APIS` is *ours*: upstream guards the PNG writer only by `MINIZ_NO_DEFLATE_APIS`, which we need, so `tools/patches/miniz/0001-guard-png-writer.patch` adds the guard. Without it the PNG entry points are exported and `test-abi.R` fails.
- `MZ_ASSERT` expands to `assert`, reachable from malformed input. R supplies `-DNDEBUG` so normal and CRAN builds compile it away, but a `-UNDEBUG` build (`devtools`' debug install) can abort the R session instead of raising a condition. Hardening this is Stage 13 work; don't be surprised by it before then.

Object files must never reach the tarball — `.Rbuildignore` excludes `src/**/*.o` and `*.so`/`*.dll`. A stale debug `.o` left in the tree will otherwise be shipped *and* be relinked in preference to a fresh compile, which surfaces as a confusing `assert`/`compiled code` check warning.

## Architecture

```
R API (komp_*)          C ABI (zu_*, via zukomp.h + R_RegisterCCallable)
        \                      /
         zukomp core: registry, stream driver, limits, errors
                        |
      identity | deflate-raw | zlib | gzip   (future: brotli, zstd, lz4, snappy)
```

**Codecs are vtables behind an enum, discovered at runtime.** `zu_codec_vtable` (design §10) supplies encoder/decoder quartets, level range, magic bytes or a sniff predicate, and metadata. Adding a codec is a new enum value plus a new vtable — no header change, no ABI bump. `zu_register_codec()` may only be called during `R_init_*`, before any stream exists.

**Limits live in the core driver, never in a codec.** `max_output` and `max_ratio` are enforced by `src/zu_stream.c`, which sees every byte through the `zu_buffer` cursors. This is why a third-party codec nobody here reviewed still cannot bypass the output cap — Stage 12's consumer-package test exists to prove exactly that.

**One axis, not two.** Wrapper variants are distinct codec identities (`deflate-raw`, `zlib`, `gzip` are three codecs, not one algorithm with a `format` argument). Levels are **codec-native** and validated against the codec's advertised range; no cross-codec numeric equivalence is claimed.

**Whole-buffer functions drive the streaming engine — there is no second code path.** `zu_int_run_whole()` in [src/zu_whole.c](src/zu_whole.c) is that one loop; `komp_compress()`, `komp_decompress()` and `zu_test_stream()` all call it. This is why the chunk-boundary sweeps are worth anything: a separate whole-buffer loop would mean every sweep tested code no user runs. It also carries the design §13 obligations in one place — output on `R_alloc` with `vmaxget`/`vmaxset`, `R_CheckUserInterrupt()` every 64 iterations, and no `Rf_error()` anywhere holding a buffer.

Planned layout: `src/{init,zu_status,zu_registry,zu_stream,zu_buf,zu_gzip,codec_identity,codec_deflate}.c`, `src/vendor/miniz/`, `inst/include/{zukomp.h,zukomp-r.h}`, `R/{codecs,compress,decompress,conditions,info}.R`, `tools/vendor/`. R-visible `.Call` entry points live in `src/zukomp_r.c`; pure-C ABI code never includes an R header.

**Adding a C source file means editing `OBJECTS` in `src/Makevars` by hand.** R auto-compiles only `src/*.c`, miniz lives in a subdirectory, and a `$(wildcard)` would force `SystemRequirements: GNU make`, which design §12 forbids. A file that is not in `OBJECTS` is silently not built.

### Naming, and it is enforced by tests

| layer | prefix |
|---|---|
| R exports | `komp_` |
| C ABI | `zu_` |
| entry points / registration | `zukomp_` (`R_init_zukomp`, `zukomp_get_api`) |
| internal only, never installed | `zu_int_` |
| test-only `.Call` symbols | `zukomp_test_` |

Never exported under any circumstances: `deflate`, `inflate`, `compress`, `uncompress`, `deflateInit`, `inflateInit`, `crc32`, `adler32`, or anything else that reads as the zlib ABI. `test-abi.R` audits this, plus the absence of any `mz_zip_*` or PNG symbol.

## Invariants that are easy to break

- **`inst/include/zukomp.h` must compile standalone as C99** against only `<stddef.h>`/`<stdint.h>`. No `R.h`, no `SEXP`, no miniz type or symbol, no DEFLATE vocabulary (`zu_encoder`/`zu_decoder`, never `deflater`/`inflater`). R-specific resolution belongs in `zukomp-r.h`. Enforced two ways, and they use the same comment-stripped rule so keep them in step: the `abi.yaml` workflow compiles the header standalone under `-Werror` (C99 and C++), and `test-abi.R` greps the *installed* copy via `installed_header_code()`. The rule targets declarations, not prose — a comment may name miniz, a declaration may not.
- **`MINIZ_NO_ZLIB_COMPATIBLE_NAMES` is not optional.** Without it `miniz.h` `#define`s `compress`, `crc32`, `adler32` and friends over every translation unit that includes it — including ones that also see R's headers, since R links its own zlib.
- **Anything holding heap state across a longjmp must be owned by R.** `Rf_error()` *and* `R_CheckUserInterrupt()` both jump straight past any `free()` below them. The stream handle in `zu_int_run_whole()` is therefore held by an external pointer with `R_RegisterCFinalizerEx(..., TRUE)` for the duration of the loop, and freed eagerly (pointer cleared first) on the normal path. This is not theoretical: the first version of that loop called `R_CheckUserInterrupt()` with the handles in bare locals, leaking one stream per interrupted decompression.
- **No `Rf_error()` inside the codec loop.** C returns `zu_status`; only the outermost `.Call` raises. `Rf_error()` and `R_CheckUserInterrupt()` both longjmp past `free()`. Growing buffers use `R_alloc` with `vmaxget`/`vmaxset`, or an external pointer with `R_RegisterCFinalizerEx(ptr, fin, TRUE)`.
- **"The trailer ended" is not "the stream ended".** A gzip member may be followed by another, so the decoder goes to `ST_MEMBER_END` and decides. Whether more input exists is only knowable at `ZU_FINISH` — before that, an empty buffer means "not yet", not "no more". A following member must start with `1f 8b`; anything else is trailing junk, which is a far more useful thing to report than a malformed member header.
- **Trailing bytes are policy, and policy lives in the options.** The codec stops exactly at the end of the stream and leaves `src_pos` exact; `zu_decoder_process()` then applies `ZU_DEC_REJECT_TRAILING`. `ZU_DEC_CONCAT_MEMBERS` likewise gates member continuation. Both default to on in `zu_test_stream()`, matching whole-buffer semantics.
- **The gzip header parser is fed one byte at a time** (`src/zu_gzip.c`). A header field can straddle any number of `process()` calls — the sweeps run at one byte per call — and byte-at-a-time is the only shape where that is obviously correct rather than merely tested. It is kept in its own file to stay separately fuzzable in Stage 14. FNAME/FCOMMENT bytes are discarded, never buffered, so a hostile multi-megabyte filename costs time and nothing else.
- **zukomp owns the zlib wrapper; miniz only does raw DEFLATE.** `codec_deflate.c` drives miniz at `window_bits = -15` and implements RFC 1950's two-byte header and Adler-32 trailer itself. This is not incidental: `mz_inflate()` reports a corrupt Adler-32 and corrupt compressed data both as `MZ_DATA_ERROR`, and design §24 criterion 6 requires checksum failures to surface distinctly as `zukomp_checksum_error`. Stage 7's gzip wrapper reuses this `ST_HEADER`/`ST_BODY`/`ST_TRAILER` structure. Every wrapper byte is streamed one at a time if that is all the room there is, because the chunk sweeps run at `in_chunk = out_chunk = 1`.
- **`zu_test_stream()` is the main test lever.** It drives the C driver at caller-chosen input/output chunk sizes (`zu_test_stream(bytes, codec, mode, in_chunk, out_chunk, max_output, max_ratio, flush_every)`), which is how chunk-boundary correctness is tested from Stage 4 rather than from Stage 16. Sweep `chunk_sizes()` — 1 is the harshest boundary.
- **Limits shrink the codec's window; they are not a post-hoc check.** `zu_decoder_process()` reduces `dst_size` to the remaining allowance so a codec physically cannot write past `max_output`. Two subtleties: landing *exactly* on the cap is not an error (the stream still needs one more call to observe `ZU_FINISH`), and it is the codec asking for **more** room that distinguishes "finished at the cap" from "would have exceeded it". `max_ratio` is checked after the fact instead, since `max_output` already bounds memory.
- **Never allocate based on a size claimed by the input.** gzip's ISIZE is validated against actual output, never used to size a buffer. All buffer arithmetic goes through checked `zu_add`/`zu_mul`/`zu_grow` — never a bare `size *= 2`.
- **`codec = "auto"` never falls back to a headerless codec.** `zu_sniff()` tries fixed magic first (longest first, so a short magic cannot shadow a longer one), then predicate sniffers, then errors `zukomp_undetectable_codec`. A codec with neither magic nor sniffer is never detected — that is what keeps `auto` off raw DEFLATE, identity and brotli, where guessing wrong means silently returning the wrong bytes. zlib's predicate has a measured ~0.1% false-positive rate on random bytes (matching the theoretical 1/(16·2·31)), which `test-detect.R` documents rather than hides.
- **Codec enum values are permanent.** A codec compiled out keeps its number and reports unavailable.
- **The ABI reaches consumers through a registered C-callable table, resolved lazily.** `LinkingTo` supplies headers, not object code, so [inst/include/zukomp-r.h](inst/include/zukomp-r.h) fetches `zukomp_get_api` with one `R_GetCCallable` and caches it. Lazily because `Imports: zukomp` does *not* load zukomp's namespace without a real `importFrom()` in the consumer's NAMESPACE — resolving at their DLL init can therefore fail. `zukomp_get_api(requested)` returns NULL on a version mismatch rather than a best guess. The resolver casts `DL_FUNC` through a **union**: a direct cast trips `-Wcast-function-type-mismatch`, which is a build failure in the *consumer's* tree, not ours — the `abi.yaml` workflow compiles a stand-in consumer with `-Werror` to catch that.
- **The registry is written exactly once**, from `R_init_zukomp` via `zu_int_register_builtin_codecs()`, and is read-only for the rest of the session. That invariant is what makes the package thread-safe and the test suite safe to run in parallel, so there is deliberately no way to register a codec from R. Stage 12's consumer package is what exercises `zukomp_codec_table()`'s branch for third-party (undeclared) codecs; nothing in zukomp's own suite can reach it.
- **A codec's name is declared independently of its implementation.** `zu_int_declared[]` in `src/zu_registry.c` lists every codec this build knows the *name* of; the registry lists the ones with a vtable. That split is what lets `komp_codecs()` show a `zstd` row with `available = FALSE` instead of pretending the codec does not exist, and it is why an unknown name is an error while a known-but-absent one is merely `FALSE`. Adding a codec means adding a row here or registering externally — never editing the header.
- **`zukomp` has no `Imports`.** Test-only dependencies (`testthat`, `withr`) live in `Suggests` and are never referenced from `R/`.
- **gzip output is deterministic** (`mtime = 0`, `OS = 255`, no filename, no comment) — scoped to a fixed zukomp version, and documented as *not* a content hash.
- Codec-specific quirks stay downstream: the `Content-Encoding: deflate` ambiguity and its retry-as-raw policy belong in `zuhttp`, not here.

### The consumer package

[tests/consumer/zukomptest](tests/consumer/zukomptest) is a package that consumes zukomp the way `zuhttp` will — `Imports` + `LinkingTo`, an `importFrom` in NAMESPACE, and its own `xor5a` codec registered at `ZU_CODEC_VENDOR_BASE` from `R_init_zukomptest`. It is `.Rbuildignore`d; only the `consumer.yaml` workflow builds it.

It exists because design §24 criteria 10 and 12 are claims that cannot be checked from inside zukomp. The load-bearing test is **"core limits apply to a third-party codec"**: a codec nobody here reviewed still cannot bypass `max_output`. If that ever fails, the security model is decorative.

To run it locally: `R CMD INSTALL .`, then `R CMD INSTALL tests/consumer/zukomptest`, then `testthat::test_local("tests/consumer/zukomptest")` — against a library where both are installed.

### Fuzzing

Six libFuzzer targets in [fuzz/](fuzz/) link the **pure-C core with no R in the process**. That is why `src/zu_internal.h` must stay free of R — R-dependent internals live in `src/zu_rglue.h`, and an `Rinternals.h` error when building the fuzzers means something leaked into the core.

```sh
./tools/make-fuzz-corpus.sh    # seed from the committed interop fixtures
./fuzz/build.sh                # libFuzzer; needs a clang that ships it
./fuzz/run.sh 60               # 60s per target
./fuzz/build.sh --standalone   # replay drivers: ASan+UBSan only
./fuzz/replay.sh               # replay the corpus as a regression check
```

Apple's clang ships **no libFuzzer runtime**, which is what `--standalone` is for: the same targets behind a `main()` that replays files, so the corpus and every committed crasher stay checkable on macOS.

Decoders fuzz under a 16 MiB output cap — without one you just rediscover decompression bombs and report them as OOM. `fuzz_gzip_header` drives the parser directly so every input is spent on the riskiest code rather than on DEFLATE.

**A fuzz finding without a regression test is a finding that can come back.** Minimise it, commit it to `fuzz/corpus/regressions/`, note it in that directory's README, and add a testthat test.

## Testing conventions

Set once in `ROADMAP.md` and inherited by every stage:

- **Self-sufficient.** Every test builds its own inputs inside the `test_that()` block; no file-scope objects. Repetition beats cleverness — a truncation failure at byte 217 should be reproducible from the failing test alone.
- **Self-contained.** Global state via `withr::local_*()`; randomised payloads via `withr::local_seed()`. `setup-state.R` installs a state inspector that catches leaked options or stray codec registrations.
- **Assert on condition classes, never message text.** `expect_error(..., class = "zukomp_checksum_error")`. Wording is covered separately by snapshots, so a rewording is one snapshot diff instead of forty broken tests.
- **Order independence.** `devtools::test(shuffle = TRUE)` is part of the definition of done; the registry makes ordering bugs plausible.
- Helpers: `new_payload(kind, n)` / `payload_kinds()` in `helper-corpus.R`; `expect_roundtrip()`, `expect_chunked_roundtrip()`, `expect_codec_error()` in `helper-expect.R`.
- **`zu_test_stream()`** — an unexported, always-compiled `.Call` harness driving the C stream driver at caller-chosen input/output chunk sizes. Chunk-boundary correctness is what `zuhttp` depends on and what silently rots, so it is tested from Stage 6 even though the R streaming API is phase 2.
- **Payload constructors must be byte-explicit, never built from source-file string literals.** `new_payload("utf8")` originally used a literal, so its bytes depended on how R decoded `helper-corpus.R`'s encoding — which varies by platform and locale, and made a fixture generated on one machine fail on another. It is spelled out byte by byte now. The gzip interop test is what caught it.
- **Interop uses committed fixtures, never external processes.** The corpus lives in `tests/testthat/fixtures/` with provenance in its `MANIFEST.tsv`; regenerate with `Rscript tools/make-fixtures.R` and verify reproducibility with `--check`. The generators source `helper-corpus.R` so a fixture's payload and the `new_payload(kind, n)` a test compares against cannot drift. `--check` treats a *different generator version* producing different bytes as expected, and only the same version doing so as a failure. Note `"random"` is deliberately absent from the corpus (it depends on R's RNG stream); `"lcg"` is the reproducible incompressible stand-in that exercises DEFLATE's stored blocks. `tools/make-fixtures.R` runs offline by a maintainer and records provenance in `tests/testthat/fixtures/MANIFEST.tsv`; tests read via `test_path()` and never regenerate. CRAN guarantees neither `gzip` nor Python.
- **Bomb tests use tiny limits**, never large allocations, to prove a cap works.
- **CRAN budget: the full suite finishes under 60 seconds.**

Deliberately outside testthat, in CI jobs: ASan/UBSan (`memcheck.yaml`, on `rhub/rocker-gcc-san`), valgrind, `rchk` for PROTECT discipline, the consumer package (`consumer.yaml`), external-decoder interop (`tools/check-interop.sh`), the standalone-header probe (`abi.yaml`), and the vendor guard (`vendor.yaml`). Fuzzing (`fuzz/`) and benchmarks (`bench/`) arrive at Stages 14 and 19. Note the sanitizer job asserts on its own log: ASan and UBSan report to stderr without changing the exit code, so a job that only checks the status is green by construction.

## Definition of done for any stage

`devtools::document()` and `devtools::check()` clean (0/0/0); `devtools::test(shuffle = TRUE)` green; CI green on all nine {OS × R version} cells; new public surface has roxygen docs with runnable examples; `tools/vendor/verify` clean if `src/vendor/` moved; the stage's own Verify block passes; suite still under 60 seconds.
