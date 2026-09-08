# zukomp: outstanding work

State at the time of writing: v1 (0.1.0) on `develop`, pushed to
`origin/develop`. `R CMD check --as-cran` is 0/0/0, 1146 test assertions
(2008 with `ZUKOMP_SLOW_TESTS=true`), 87 consumer-package assertions.

The seven findings from the code-review pass are **already fixed** in
`1568084` and are not repeated here. What follows is what is still open,
ordered by what would hurt soonest.

---

## 1. Gaps that will bite `zuhttp` first

These are the parts of the published API that `zuhttp` is documented to
rely on and that nothing currently exercises. None is known to be broken —
that is the problem: nothing would tell us if it were.

### 1.1 The abstract level names from design §4 are not implemented

Design §4 specifies `"fast"`, `"default"` and `"best"` as **"the portable
way to express intent"**, and says codecs without a level axis accept
`"default"`. None of them works:

```r
komp_compress(x, "gzip", level = "fast")
#> Error: `level` must be a single whole number, or NULL for the codec's default.
```

This is v1 surface that was specified and never built. It matters more than
a convenience usually would, because the design's whole position on levels
is that **numeric levels are codec-native and not comparable across
codecs** — the strings are the only cross-codec way to say "compress
harder". A caller writing codec-agnostic code today has no correct option.

*Do:* implement the three names in `zu_check_level()`, mapping through each
codec's advertised `[level_min, level_default, level_max]`. Add a
`level = "default"` path for codecs with no level axis. Either that, or
strike the feature from design §4 — but it should not silently stay
unbuilt.

### 1.2 `zu_encoder_reset()` — partly closed, and it was broken

**Closed for the encoder.** `zukomp_test_encoder_reset()` now drives it and
three tests pin the behaviour. Writing them found the bug the item
predicted: `mz_deflateReset()` re-runs `tdefl_init()` with the flags baked
in at `mz_deflateInit2()` time, so a reset with a new level changed the
zlib header's FLEVEL bits and not the payload. Fixed by re-initialising
miniz when the level actually changes.

**Still open for the decoder.** `zu_decoder_reset()` has no caller outside
its own definition. The state it must get right is larger than the
encoder's — `total_in`/`total_out` (and so every limit budget), the wrapper
state machine, the gzip header parser and miniz's own stream.

*Do:* add a reset path to `zu_test_stream()` (e.g. `reset_after`), then
test that a reset stream decodes a second message identically to a fresh
one, that the limit budget restarts rather than carrying over, and that
reset across a *different* codec is refused.

### 1.3 `ZU_FLUSH` is only tested against `identity`

`test-stream.R:45` is the sole `flush_every` test and it uses `identity`,
for which flush is trivially a no-op. The real path — `MZ_SYNC_FLUSH`
through miniz, for gzip and zlib — is untested.

Design §8: *"`ZU_FLUSH` exists because a boolean `finish` cannot express
'put the bytes on the wire now' — needed the moment `zuhttp` compresses a
streaming request body."*

*Do:* extend the flush test across `zlib`, `gzip` and `deflate-raw`.
Assert the useful property: output after intermediate flushes still decodes
to the same input (it will not be byte-identical to unflushed output, and
should not be expected to be).

### 1.4 The one-shot API — partly closed, and it was broken

**Closed for `zu_decompress_one()`.** `zu_test_decompress_one()` drives it
against all four codecs at an exact capacity, one byte short, and at zero.
That found two bugs: a decoder with no output room could never observe the
end of its stream (an empty payload into a zero-byte sink was reported as
an output-limit error), and `mz_inflate()`'s MZ_FINISH-on-first-call fast
path marks a stream permanently failed when the sink is too small, so an
undersized buffer was reported as *corrupt input*. Both fixed.

**Still open:** `zu_compress_bound()` and `zu_compress_one()` still run only
against `xor5a` in the consumer package — a codec with no wrapper, no
expansion and `bound(n) == n`. `bound()` has never been checked for gzip or
zlib, where it must leave room for a header, a trailer and stored-block
overhead.

*Do:* test the compress pair against all four codecs, including the
incompressible `lcg` payload where `bound()` is closest to being wrong.

### 1.5 `can_flush` is not discoverable from R

The vtable carries `ZU_CAN_FLUSH` and the C ABI exposes it through
`zu_codec_info.flags`, but `komp_codecs()` returns only the ten columns
design §6 listed, which omit it. An R-level caller cannot ask whether a
codec supports flushing before trying.

*Do:* either add a `can_flush` column (and amend design §6), or document
that flush capability is a C-level concern only.

---

## 2. Known hazards, deliberately deferred

### 2.1 `MZ_ASSERT` can abort the R session

miniz has 26 `MZ_ASSERT` call sites reachable from malformed input, and
`MZ_ASSERT` expands to `assert()`. R supplies `-DNDEBUG` so ordinary and
CRAN builds compile them away — but a `-UNDEBUG` build, which is what
`devtools`' debug install uses, will `abort()` the whole R session instead
of raising a condition.

`MZ_ASSERT` is defined unconditionally in `miniz.h`, so it cannot be
overridden from the command line. Deferred at Stage 13 and still open.

*Do:* extend `tools/patches/miniz/` to make it `#ifndef MZ_ASSERT`-guarded,
then define it to a no-op (or to something that records a status) in
`src/Makevars`. This is the last place in the package where hostile input
can terminate the process rather than return an error.

### 2.2 `buffer_cap` is a dead parameter — **closed**

Deleted, along with `zu_int_grow()`'s `cap` parameter, which nothing else
set either. `zu_int_reserve()` now asks `zu_int_grow()` for the actual
shortfall rather than for `extra`, so the latent off-by-one is gone rather
than merely harmless. `max_output` in the driver is the only bound on
decompressed size, which is what design §20 intends.

---

## 3. Performance

### 3.1 Whole-buffer decompression peaks at ~3× the output size

Measured: decoding 64 MB costs ~199 MB of R peak memory, a 3.1× ratio.

`R_alloc` cannot resize, so `zu_int_reserve()` grows by allocating a new
block and copying — and the old block stays on the `vmax` stack until the
enclosing `.Call` returns. With doubling, several superseded blocks are
live at once.

This is correct and safe, and it is the reason `komp_decompress()`'s
default cap is 1 GiB rather than something larger; but a caller decoding a
large body pays for it. (`zuhttp` should not: it will use the incremental
C path, which reuses one sink.)

*Do:* for **compression**, size the buffer once from `zu_compress_bound()`
and skip growth entirely — the bound is exactly what it is for. For
decompression, consider an external-pointer-owned `malloc`/`realloc` buffer
with a finalizer, which is already the ownership pattern used for stream
handles.

### 3.2 No benchmarks

Design §22 decision 11 says to benchmark against system zlib and
`memCompress()`. Nothing exists. Phase 2 item 19.

---

## 4. Cleanup

- **`src/zu_miniz.c` is Stage 1 scaffolding.** `zukomp:::zu_miniz_version()`
  is fully redundant with `komp_info()$vendored`. Delete the file, its
  `.Call` registration, `R/miniz.R`, and repoint `komp_info()` and
  `test-abi.R` at a single source.
- **`komp_info()$version` returns a `package_version` object**, not a
  string, which is slightly awkward to `paste()` into a log line. Consider
  `as.character()`.

---

## 5. CI and infrastructure

### 5.1 No workflow has ever run

This is the largest unknown in the whole package. All seven workflows —
`R-CMD-check`, `pkgdown`, `vendor`, `abi`, `consumer`, `fuzz`,
`native-checks` — were written without a single execution, because the repo
had no remote until the first push. Expect first-run failures: YAML
details, missing system dependencies, container image names, and the
`kalibera/rchk` invocation in particular.

Verified locally instead: the vendor guard, the ABI header probes, the
consumer build and tests, `tools/check-interop.sh`, and the fuzz targets
via the standalone ASan+UBSan replay path. The libFuzzer, valgrind, rchk
and gctorture *jobs* are unproven even though the underlying checks were
run by hand.

*Do:* watch the first run and treat it as part of the release, not as
finished work.

### 5.2 Upstream bug in `pedrobtz/r-actions` sanitizers workflow

`.github/workflows/sanitizers.yml` in that repo sets `CC`, `CFLAGS`,
`CXXFLAGS` and `LDFLAGS` as **environment variables**. `R CMD INSTALL`
ignores them: R's `etc/Makeconf` assigns `CFLAGS` with `=`, and make
prefers a makefile assignment over the environment. Verified here with a
sentinel `-D` — absent via the environment, present via `~/.R/Makevars`.

That job is very likely building uninstrumented and therefore cannot fail.
Separately it sets `UBSAN_OPTIONS=print_stacktrace=1` without
`halt_on_error=1`, so UBSan findings print to stderr and leave the job
green — and both bugs fuzzing found in this package were UBSan findings of
exactly that kind.

*Do:* in `r-actions`, write the flags to `~/.R/Makevars` (as `lto.yml`
already does correctly), add `halt_on_error=1`, and assert the flags
reached the compiler. Consider adding a `workflow_call` `inputs:` block so
callers can pass env like `ZUKOMP_SLOW_TESTS`.

### 5.3 `native-checks.yaml` carries a job that should be temporary

`sanitizers-exhaustive` exists only because the shared workflow takes no
inputs and does not halt on UBSan. Delete it once 5.2 is fixed — it is
commented to say so.

### 5.4 Coverage is not wired up

`pedrobtz/r-actions` also provides `coverage.yml`; this repo does not use
it. Given how much of the package is C reached through a small R surface,
line coverage would mostly measure the test harness — but branch coverage
of `R/` would still be informative.

---

## 6. Documentation and release readiness

- **No `_pkgdown.yml`, and no site.** `DESCRIPTION` used to advertise
  `https://pedrobtz.github.io/zukomp/`, which is a 404: `pkgdown.yaml`
  deploys only on push to `main`, there is no `gh-pages` branch, and all the
  work is on `develop`. CRAN's incoming check flags a 404 URL, so the URL
  has been **removed from `DESCRIPTION`** for now. Put it back once the site
  actually deploys.
- **No vignette.** For a package whose central claim is an extensibility
  model, "how to write a satellite codec" is the vignette that would earn
  its place — the consumer package is already a worked example.
- **`cran-comments.md` is written** and `.Rbuildignore`d. CRAN incoming
  checks have now been run with network access
  (`_R_CHECK_CRAN_INCOMING_REMOTE_=true`): the only remaining NOTE is the
  expected "New submission". Before submitting, add win-builder and
  macbuilder results to that file — neither can be run locally.
- **`NEWS.md` is written for 0.1.0** and will need the usual discipline
  from here.

---

## 7. Design-document debt

- **Design §24 criterion 11 is open** and cannot be closed here: it names
  `zuhttp`, which is still an empty skeleton. `tests/consumer/zukomptest`
  proves zukomp *supports* incremental decoding (5 MB through a reused
  4 KiB sink); the criterion itself needs a real client. Recorded in both
  `.agents/` documents.
- **Three design claims were corrected during implementation** and should
  be read as amended, not as originally written: the six-define miniz trim
  (§12), the `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` rationale (§12), and the
  `Imports:`/`importFrom` namespace-loading claim (§15).
- **`zukomp_invalid_argument` is not in design §7's condition hierarchy**
  but is raised throughout. Add it to the list.

---

## 8. Repository

- **The default branch on GitHub is `develop`**, because that was the first
  branch pushed. `main` now exists but points at the initial skeleton
  commit. If the intended convention is `main` as default, change it in the
  repo settings before the PR is merged.
- **No release tag.** `v0.1.0` should be tagged once the first CI run is
  green.
