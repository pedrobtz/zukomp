# zukomp review — consolidated findings (2026-11)

Merges two independent reviews of `main` at `343cd97`, kept verbatim beside it:

- [2026-11-09-codex.md](2026-11-09-codex.md) — two passes, probes run in a
  temporary copy (ASan, native reproductions, `R CMD INSTALL --preclean`).
  Cited below as **R1**.
- [2026-11-09-vibe.md](2026-11-09-vibe.md) — correctness-only code
  read, static analysis. Cited below as **R2**.

Where both reviews found the same issue it appears once, with both sources.
"Confirmed" means one of the reviews reproduced it; "inspection" means it was
derived from the code only.

Baseline checks R1 ran, all passing: clean install; package tests (4,188
assertions, seven slow tests skipped); `tools/zukomptest` (205 assertions);
`tools/zukomplink` build/link/extract/run-without-zukomp; vendor
reconstruction (all 76 files reproduced exactly).

## Summary

| # | Finding | Severity | Status | Source |
|---|---|---|---|---|
| 1 | DEFLATE encoder silently truncates input > 2^31−1 bytes | high | confirmed | R1, R2 |
| 2 | Malformed DEFLATE (missing EOB) classified by delivery, not data | high | confirmed | R1, R2 |
| 3 | `zu_register_codec()` reads a short vtable before checking `struct_size` | medium (memory safety) | confirmed (ASan) | R1, R2 |
| 4 | `libzukomp.a` / `miniz_zip.o` stale even with `--preclean` | medium (build) | confirmed | R1 |
| 5 | LZ4 block decoder uses the compressor's input ceiling | medium | confirmed | R1 |
| 6 | Stream handle has no owner between `_new` and extptr/finalizer | medium (leak) | inspection | R1, R2 |
| 7 | Frame/member lookahead consumes bytes it cannot return | medium (contract) | known deviation | R1, R2 |
| 8 | `max_ratio` acceptance depends on output-window size | medium (policy) | confirmed | R1 |
| 9 | Fuzzing drifted from shipping build and R driver | medium (assurance) | inspection | R1 |
| 10 | Safety tests that can pass without exercising their property | medium (assurance) | inspection | R1 |
| 11 | Decoder state after an error is unspecified | low (API contract) | confirmed | R1 |
| 12 | `zukomp-r.h` documents a `NULL` fallback that `R_GetCCallable` cannot give | low (docs) | confirmed | R1 |
| 13 | LZ4 frame `compress_bound` returns a tiny "OK" for huge sizes | low | confirmed | R1 |
| 14 | miniz patch 0002: `m_total_out` wraps on 32-bit | low (portability) | inspection | R1, R2 |
| 15 | Follow-ups: per-codec memory bounds, upstream rebuild, static-consumer fixes | — | — | R1 |

Suggested first tickets: **1, 2, 3** (concrete reproductions, narrow fixes),
then **4** (it can ship a stale archive to `zuxlsx`).

No issue is filed per finding yet. Open issues these touch: #12 (alloc-failure and arch workflows: findings 6 and 14), #65 (limit straddles: finding 8), #68 (fuzz oracles: finding 9), #71 (post-error contract: finding 11), and `.agents/assurance.md`, whose `LZ4_MAX_INPUT_SIZE` row is marked `n/a` on the strength of the refusal finding 5 says is the bug.

---

## 1. DEFLATE encoder silently truncates input > 2^31−1 bytes

**Where:** `src/codec_deflate.c:395` (input clamp), `:412` (flush
passthrough), `:451` (`STREAM_END` accepted without checking consumption).

`deflate_process()` clamps `mz.avail_in` to `0x7FFFFFFF` but still passes
`ZU_FINISH` as `MZ_FINISH`. The whole-buffer driver offers the entire input
in one chunk, so for `n >= 2^31` miniz treats the clamped slice as the whole
stream, emits the final block and returns `MZ_STREAM_END`. The trailer
(CRC-32/ISIZE or Adler-32) is accumulated over the slice only, so the output
is **self-consistent with the truncated prefix** — no downstream integrity
check can detect the loss. `out.consumed` is correct but nobody checks it.

R1 reproduced natively via `zu_compress_one()`: 2147483648 input bytes →
`ZU_OK`, gzip ISIZE 2147483647. R1 did not run it through the R wrapper; R2
derives that `komp_compress()` takes the same path (`zu_int_whole()` sets
`in_chunk = n`). Affects `gzip`, `zlib`, `deflate-raw`.

**Fix:** when the encoder has input beyond the miniz slice, pass
`MZ_NO_FLUSH` instead of `MZ_FINISH`:

```c
int mz_flush = zu_int_to_mz_flush(flush);
if (s->encoder && avail_in > 0x7FFFFFFFu) {
    mz_flush = MZ_NO_FLUSH;   /* more input remains: this is not the last call */
}
```

Plus an invariant in `zu_int_one_shot()`: an encode that reaches
`ZU_STREAM_END` with `src_pos != src_size` is `ZU_ERR_INTERNAL`.

**Test:** a cheap CI variant that does not allocate 2 GiB (shrink the clamp
behind a test-only macro, or hand-feed a `zu_buffer`), asserting
`STREAM_END` only with `src_pos == src_size`; a slow-gated boundary sweep
`n = 2^31−3 .. 2^31+2` through `zu_compress_one()` and `komp_compress()`.

## 2. Malformed DEFLATE (missing end-of-block) is classified by delivery

**Where:** `src/codec_deflate.c:519-540` (stall classification) and miniz
tinfl's lack of an end-of-block presence check; pinned as expected in
`tests/testthat/test-malformed.R` (R1 cites `:95`, R2 `:70`).

With the 36-byte `raw-missing-end-of-block` fixture:

- whole input at once → `zukomp_invalid_data`, zero output;
- one-byte input chunks → 8,192 bytes of output, then `zukomp_output_limit`.

The cap contains the damage, but the error class depends on chunking, a
caller that treats `output_limit` as "retry with a bigger cap" decodes
garbage, and native callers can disable the cap entirely. CLAUDE.md's
"Bomb tests" bullet ("not a structural error at all") is wrong: zlib rejects
this structure explicitly
([inflate.c#L945-L950](https://github.com/madler/zlib/blob/v1.3.1/inflate.c#L945-L950)).

**Fix:** short term, in the ST_BODY decoder map "`ZU_FINISH`, input spent,
no progress, never reached `MZ_STREAM_END`" to `ZU_ERR_INVALID_DATA` before
the driver's output-cap check wins. Proper fix: an EOB-presence /
Huffman-table completeness check in miniz as a new `tools/patches/miniz/`
patch (four files: patch, manifest, `inst/COPYRIGHTS`, `cran-comments.md`).
Correct the CLAUDE.md bullet in the same commit.

**Test:** sweep `in_chunk = 1..length(x)` at a small `max_output`; assert
one class (`zukomp_invalid_data`) and `raw(0)` output at every chunk size.
Extend to every `fixtures/malformed` vector (one class per vector).

## 3. `zu_register_codec()` reads a short vtable before checking `struct_size`

**Where:** `src/zu_registry.c:94` reads `vtable->name` (offset 8) and
`vtable->source` (offset 40) before the `struct_size` check at `:108`.

A genuinely short allocation (e.g. `malloc(4)` holding only `struct_size`)
gives an out-of-bounds read instead of `ZU_ERR_INVALID_ARGUMENT`; R1
confirmed with ASan. Reachable from any satellite at DLL-load time. The
existing tests shorten `struct_size` on a full-size struct, which cannot
expose an OOB read. `zu_int_copy_opts()` already does this correctly.

**Fix:** check `vtable != NULL`, then `struct_size >=
ZU_VTABLE_REQUIRED_SIZE`, then the pointer fields.

**Test:** in `tools/zukomptest` (or a sanitizer-only C test), register a
real short allocation; expect `ZU_ERR_INVALID_ARGUMENT` with no ASan report.

## 4. Static archive stays stale even with `--preclean`

**Where:** `src/Makevars:194`.

`miniz_zip.o` depends only on `miniz.c` and is outside `OBJECTS`, so R's
`shlib-clean` never removes it. R1 changed miniz's version macro in the
header and ran `R CMD INSTALL --preclean`:

```text
shared object miniz: review-stale-probe
static archive miniz: 11.3.2
```

Compiler/flag changes hit the same problem, and an ordinary incremental
install can ship an updated `miniz.h` beside the old compiled ABI — exactly
the silent layout mismatch the `MINIZ_NO_TIME` rule warns about, delivered
to `zuxlsx`.

**Fix:** declare header dependencies for `miniz_zip.o`; extend `shlib-clean`
to remove `miniz_zip.o` and `libzukomp.a`. Must stay POSIX make (no
`SystemRequirements: GNU make`).

**Test:** build, touch/change a vendored header, rebuild with `--preclean`,
compare `mz_version()` from the archive against `komp_info()$vendored`
(`tools/check-linking.sh` already has that comparison).

## 5. LZ4 block decoder uses the compressor's input ceiling

**Where:** `src/codec_lz4.c:467` and the direct path at `:660`.

Both encode and decode limit input to `LZ4_MAX_INPUT_SIZE`, which bounds
*uncompressed* compressor input (2,113,929,216). A valid compressed block
can be up to `LZ4_compressBound` of that (2,122,219,150). R1 built a large
literal-only block that upstream's partial decoder accepts and zukomp
rejects before consuming anything.

**Fix:** separate limits for compression input and decoder compressed input,
checking every narrowing to `int`.

## 6. Stream handle has no owner between creation and finalizer registration

**Where:** `src/zu_whole.c:172`/`:182` (`zu_encoder_new`/`zu_decoder_new`)
vs `:189` (`R_MakeExternalPtr` + `R_RegisterCFinalizerEx`).

If the extptr allocation or finalizer registration longjmps, the native
stream (and its miniz dictionary / ZSTD / LZ4F contexts) leaks permanently.
Same file already uses the right idiom for the output sink
(`zu_int_outbuf_owner()`).

**Fix:**

```c
SEXP guard = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
R_RegisterCFinalizerEx(guard, r->encode ? zu_int_encoder_finalizer
                                        : zu_int_decoder_finalizer, TRUE);
st = r->encode ? zu_encoder_new(&enc, &opts) : zu_decoder_new(&dec, &opts);
if (st != ZU_OK) { UNPROTECT(1); return st; }
R_SetExternalPtrAddr(guard, r->encode ? (void *) enc : (void *) dec);
```

**Test:** needs R allocation-failure injection (fail the Nth allocation);
gctorture does not reach it. Neither review fault-injected it. Label the PR
`full-ci` (PROTECT-sensitive glue).

## 7. Frame/member lookahead consumes bytes it cannot return (known deviation)

**Where:** `src/zu_frames.c:114` (`memcpy(f->held, p, k); buf->src_pos += k;`)
and disproof paths at `:131-147`; gzip's `probe_magic` hold in
`codec_deflate.c` `ST_MEMBER_END`; pinned in
`tests/testthat/test-members.R:293`.

After a zstd/LZ4 frame, up to three bytes that might begin another frame's
magic are consumed into `f->held`; if disproved they stay counted, so
`src_pos` can run up to three bytes past the true end (one for gzip). R is
unaffected (trailing policy still fires); a native consumer resuming the next
message from the reported cursor — the keep-alive HTTP case — loses bytes.

**Fix:** a contract decision, not a silent refactor (chunk-size sweeps pin
current behaviour): either (a) return a pending prefix to the caller on
`ZU_STREAM_END`, or (b) document that `src_pos` is exact except within three
bytes of a frame end at a chunk boundary. Decide before promising exact
stream boundaries in design §16.

## 8. `max_ratio` acceptance depends on output-window size

**Where:** `src/zu_stream.c:405` checks cumulative ratio after each
`process()`.

Payload: 200 KB zeros then 200 KB random (final ratio ≈ 2:1),
`max_ratio = 3`:

| Output window | gzip, zstd, lz4-frame |
|---|---|
| 4,096 bytes | rejected |
| 1,000,000 bytes | accepted |

`lz4-block` accepted both (it gathers all input first).

**Fix:** decide whether the limit is on the final ratio or on expansion
during streaming; document it, including that buffer size affects
acceptance; test mixed-compressibility payloads.

## 9. Fuzzing has drifted from the shipping build and the R driver

- `fuzz/build.sh:48` omits `MINIZ_NO_ASSERT`, which `src/Makevars` defines.
- `fuzz/fuzz_common.h:106` sends `ZU_FINISH` only after the last buffer is
  consumed; the R driver sends it *with* the final bytes. The DEFLATE adapter
  branches on a first-call `FINISH`, so both sequences matter.

**Fix:** fuzz both finalization orders; check build-flag parity mechanically
(e.g. from `tools/vendor/verify` or the manifest's `defines` column); add
encoder round-trip targets. Keep an assertions-on build as an *extra*
diagnostic, not the only one.

## 10. Safety tests that can pass without exercising their property

- `tests/testthat/test-memory.R:35` accepts success **or any error** as
  evidence of interruptibility — passes with no interrupt.
- `.github/workflows/fuzz.yaml:102` accepts any failing MSan canary run as
  detection, including an unrelated startup failure.

**Fix:** trigger the interrupt deterministically and assert the expected
condition plus native cleanup (`zu_test_outbuf_live()`); require the
canary's `MemorySanitizer: use-of-uninitialized-value` diagnostic.
Mutation-check each: remove the protection and confirm the test fails. (Same
rule as CLAUDE.md's "a guard whose condition the test's own subject can
satisfy is not a guard".)

## 11. Decoder state after an error is unspecified

**Where:** `src/zu_stream.c:364` marks the decoder finished before checking
the expected size. Two-byte identity stream, `expected_size = 3`:

```text
first process():  ZU_ERR_INVALID_DATA
second process(): ZU_STREAM_END
```

**Fix:** state in `zukomp.h` that a stream must be reset after an error, and
make the error sticky until reset. Test for expected-size, checksum, ratio
and trailing-data failures.

## 12. `zukomp-r.h`'s `NULL` fallback is inaccurate

**Where:** `inst/include/zukomp-r.h:98` says resolution returns `NULL` when
zukomp is unavailable. `R_GetCCallable()` raises
`function 'zukomp_get_api' not provided by package 'zukomp'` before the null
check runs
([Rdynload.c#L1643-L1653](https://github.com/wch/r-source/blob/trunk/src/main/Rdynload.c#L1643-L1653)).

**Fix:** document that the namespace/DLL must already be loaded and that
`NULL` means ABI-version mismatch only. Fix alongside the header's stale
`Imports:` claim (CLAUDE.md, #39). Test missing registration separately from
version mismatch in `tools/zukomptest`.

## 13. LZ4 frame `compress_bound` returns a tiny success for huge sizes

**Where:** `src/codec_lz4.c:385` trusts `LZ4F_compressFrameBound`, which
narrows its block count to `unsigned`:

```text
zu_compress_bound(lz4-frame, input_size = 2^48) → ZU_OK, bound = 27
```

**Fix:** reject unsupported sizes or compute a checked conservative bound.
Add extreme-value tests for every codec's `bound` — pure size queries, cheap.

## 14. miniz patch 0002: `m_total_out` wraps on 32-bit

**Where:** `tools/patches/miniz/0002-validate-match-distance.patch:73`,
applied at `src/vendor/miniz/miniz.c:2923`; field at `miniz.h:1056`.

`m_total_out` is `size_t`; on ILP32 it wraps after 4 GiB cumulative output
through one stream, after which valid matches can be rejected or a distance
under-validated. The patch comment's "saturating is not needed" is false
there. Reachable only by a 32-bit C consumer streaming > 4 GiB. R2 verified
the 64-bit semantics clean (window cap 32 KiB, reserved codes 30/31,
reset via `tinfl_init()`, first-call `FINISH` rewrite interplay).

**Fix:** saturate at the window, which is all it is ever `MZ_MIN`-ed
against:

```c
r->m_total_out = MZ_MIN(r->m_total_out + (size_t)(pOut_buf_cur - pOut_buf_next),
                        out_buf_size_mask + 1);
```

A patch change touches four files (see CLAUDE.md, *Vendored sources*).

## 15. Follow-ups (no defect yet)

- **Output limits are not memory limits.** `src/codec_lz4.c:458` buffers
  compressed block input independently of `max_output`. Document memory
  bounds per codec; consider a buffered-input limit for network consumers.
- **Automate upstream reconstruction.** The vendor guard checks recorded
  hashes; periodically fetch, apply patches and diff the tree.
- **Static consumers and security fixes.** A zukomp fix does not reach miniz
  already linked into a downstream binary. README says so; define an explicit
  "rebuild `zuxlsx`" step in the release process.

---

## Checked and found clean (R2)

- SEXP contracts on all exported functions and the test harness: type,
  length, NA, Inf, narrowing handled before C.
- PROTECT balance on all normal and error paths (gap is #6).
- All buffer arithmetic through `zu_buf.c`'s checked helpers (the one `int`
  boundary is #1).
- All 24 `R_CallMethodDef` entries match the `.Call` sites;
  `.registration = TRUE`, `R_useDynamicSymbols(FALSE)`,
  `R_forceSymbols(TRUE)` consistent; Makevars does not override
  `CFLAGS`/`LDFLAGS`; xxHash namespacing consistent.
- Vendored patches 0001/0003 and the zstd/lz4 guards are hygiene only.
- Gzip `1f 00` tail, zlib FCHECK/FDICT, `mz_inflateReset` resetting
  `m_has_flushed`, LZ4F staging sizing, zstd `windowLogMax` clamp, lz4-block
  size search, registry duplicate name/token rejection.

Design choices both reviews say to keep: C core separated from R glue,
hidden symbols, separate xxHash namespaces, versioned function tables,
recorded patches, installed license notices, interop fixtures, real consumer
packages.

## Test additions worth pinning (R2)

- `komp_compress()` at `0 / 1 / 2^31−1 / 2^31 / 2^31+1` bytes for every
  DEFLATE codec (#1; slow-gated, compressible pattern).
- Limit vectors `c(1, NA)`, `NaN`, `Inf`, `-Inf`, `0.5` for `max_output`,
  `max_ratio`, `level`; assert classes.
- Silent-coercion traps: `max_output = "1024"`, `codec = 1`, `level = "6"`,
  `x = list()` → `zukomp_invalid_argument` before C.
- Zero-length input on every codec for compress and `komp_detect()`.
- Delivery splits: `1f 8b` across every boundary; zstd magic split; `28 b5 2f`
  tails; lone trailing `1f`; XLEN 0/1/65536; FNAME/FCOMMENT split at every
  byte; FHCRC wrong at byte 1 vs 2.
- FDICT zlib: `komp_detect()` says `"zlib"`, `komp_decompress()` raises
  `zukomp_unsupported_codec`.
- Every malformed fixture at every chunk size, one class per vector (#2).
- 32-bit `m_total_out` wrap behind `#if SIZE_MAX < UINT64_MAX` (#14).
