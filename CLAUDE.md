# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`zukomp` is an R package: a **codec registry with a uniform byte-in/byte-out compression API**, backed by vendored C sources (miniz first) and no system libraries. It exists to serve `zuhttp` and future `zu*` packages through a stable, registered C-callable ABI.

The framing that governs every design decision: **zukomp is a codec registry that ships with DEFLATE, not a DEFLATE package with room for extras.** When a choice would make DEFLATE special, it is the wrong choice.

## Current state

**Stage 0 is complete**; Stage 1 (vendor miniz) is next. `DESCRIPTION` is filled in, `src/init.c` registers an empty routine table with `R_useDynamicSymbols(dll, FALSE)` and `R_forceSymbols(dll, TRUE)`, `NAMESPACE` carries the `useDynLib` directive, and `devtools::check()` is 0/0/0. CI workflows (`R-CMD-check`, `pkgdown`) are in place.

There is no codec, no registry, and no C beyond `init.c` yet — every architectural claim below describes the target design, not shipped code.

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

Once vendoring exists, `tools/vendor/verify` must pass whenever anything under `src/vendor/` moves, and CI rejects a `src/vendor/` diff that does not update `tools/vendor/manifest.tsv`.

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

**Whole-buffer functions drive the streaming engine — there is no second code path.**

Planned layout: `src/{init,zu_status,zu_registry,zu_stream,zu_buf,zu_gzip,codec_identity,codec_deflate}.c`, `src/vendor/miniz/`, `inst/include/{zukomp.h,zukomp-r.h}`, `R/{codecs,compress,decompress,conditions,info}.R`, `tools/vendor/`.

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

- **`inst/include/zukomp.h` must compile standalone as C99** against only `<stddef.h>`/`<stdint.h>`. No `R.h`, no `SEXP`, no miniz type or symbol, no DEFLATE vocabulary (`zu_encoder`/`zu_decoder`, never `deflater`/`inflater`). R-specific resolution belongs in `zukomp-r.h`.
- **`MINIZ_NO_ZLIB_COMPATIBLE_NAMES` is not optional.** Without it `miniz.h` `#define`s `compress`, `crc32`, `adler32` and friends over every translation unit that includes it — including ones that also see R's headers, since R links its own zlib.
- **No `Rf_error()` inside the codec loop.** C returns `zu_status`; only the outermost `.Call` raises. `Rf_error()` and `R_CheckUserInterrupt()` both longjmp past `free()`. Growing buffers use `R_alloc` with `vmaxget`/`vmaxset`, or an external pointer with `R_RegisterCFinalizerEx(ptr, fin, TRUE)`.
- **Never allocate based on a size claimed by the input.** gzip's ISIZE is validated against actual output, never used to size a buffer. All buffer arithmetic goes through checked `zu_add`/`zu_mul`/`zu_grow` — never a bare `size *= 2`.
- **`codec = "auto"` never falls back to a headerless codec.** Magic-first, then predicate sniffers, then error `zukomp_undetectable_codec`. Raw DEFLATE and brotli must be requested by name.
- **Codec enum values are permanent.** A codec compiled out keeps its number and reports unavailable.
- **`zukomp` has no `Imports`.** Test-only dependencies (`testthat`, `withr`) live in `Suggests` and are never referenced from `R/`.
- **gzip output is deterministic** (`mtime = 0`, `OS = 255`, no filename, no comment) — scoped to a fixed zukomp version, and documented as *not* a content hash.
- Codec-specific quirks stay downstream: the `Content-Encoding: deflate` ambiguity and its retry-as-raw policy belong in `zuhttp`, not here.

## Testing conventions

Set once in `ROADMAP.md` and inherited by every stage:

- **Self-sufficient.** Every test builds its own inputs inside the `test_that()` block; no file-scope objects. Repetition beats cleverness — a truncation failure at byte 217 should be reproducible from the failing test alone.
- **Self-contained.** Global state via `withr::local_*()`; randomised payloads via `withr::local_seed()`. `setup-state.R` installs a state inspector that catches leaked options or stray codec registrations.
- **Assert on condition classes, never message text.** `expect_error(..., class = "zukomp_checksum_error")`. Wording is covered separately by snapshots, so a rewording is one snapshot diff instead of forty broken tests.
- **Order independence.** `devtools::test(shuffle = TRUE)` is part of the definition of done; the registry makes ordering bugs plausible.
- Helpers: `new_payload(kind, n)` / `payload_kinds()` in `helper-corpus.R`; `expect_roundtrip()`, `expect_chunked_roundtrip()`, `expect_codec_error()` in `helper-expect.R`.
- **`zu_test_stream()`** — an unexported, always-compiled `.Call` harness driving the C stream driver at caller-chosen input/output chunk sizes. Chunk-boundary correctness is what `zuhttp` depends on and what silently rots, so it is tested from Stage 6 even though the R streaming API is phase 2.
- **Interop uses committed fixtures, never external processes.** `tools/make-fixtures.R` runs offline by a maintainer and records provenance in `tests/testthat/fixtures/MANIFEST.tsv`; tests read via `test_path()` and never regenerate. CRAN guarantees neither `gzip` nor Python.
- **Bomb tests use tiny limits**, never large allocations, to prove a cap works.
- **CRAN budget: the full suite finishes under 60 seconds.**

Deliberately outside testthat, in CI jobs: fuzzing (`fuzz/`), ASan/UBSan/MSan, valgrind, `rchk`, cross-package ABI consumption, external-decoder interop (`tools/check-interop.sh`), benchmarks (`bench/`).

## Definition of done for any stage

`devtools::document()` and `devtools::check()` clean (0/0/0); `devtools::test(shuffle = TRUE)` green; CI green on all nine {OS × R version} cells; new public surface has roxygen docs with runnable examples; `tools/vendor/verify` clean if `src/vendor/` moved; the stage's own Verify block passes; suite still under 60 seconds.
