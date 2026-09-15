# zukomp: outstanding work

## Current package audit (2026-09-14)

Review target: commit `95bc8b7` on `main`, matching `origin/main`. This
was a whole-package review, not a review of one diff. It covered the R
API, native codec and registry layers, the installed C API, the consumer
fixture, tests, fuzz replay, package metadata, documentation and release
checks.

Verification at the reviewed commit:

- `R CMD check --as-cran --no-manual` passed with only the expected
  new-submission NOTE.
- The full slow test suite passed 2,558 assertions with no failures,
  warnings or skips.
- `./tools/vendor/verify` passed.
- Standalone AddressSanitizer + UndefinedBehaviorSanitizer fuzz replays
  built all six targets and replayed all 47 corpus inputs successfully.

Four defects were nevertheless reproduced. The first two are P1 because
they defeat a security limit or make satellite codec availability depend
on package load order. The latter two are P2 correctness/extensibility
defects. The sections below specify proposed fixes and the regression
coverage required to close each item.

> **Status: all four are fixed**, in PR \#6 (`review-followups`). The
> per-finding sections below are kept as written — they are the
> analysis, and the reasoning is worth more than the checkbox — but read
> them as history rather than as work outstanding. Each now carries a
> **Resolution** note saying what was actually done and where it
> diverged from the recommendation. The “Earlier backlog snapshot”
> further down is also largely closed by the same PR; see its own note.

| Priority | Finding | Main consequence | Status |
|----|----|----|----|
| P1 | Fractional decompression limits narrow to another value, including the unlimited sentinel | A caller-supplied resource limit can be silently disabled | **Fixed** (#6) |
| P1 | Codec-table cache invalidation keys on visible rows rather than registry mutation | A newly loaded implementation for a declared codec remains unusable from the R API | **Fixed** (#6) |
| P2 | Gzip concatenation probes only the first magic byte | Trailing data beginning with `0x1f` bypasses the requested trailing-byte policy | **Fixed** (#6) |
| P2 | Codec registration permits duplicate names and invalid ID/name pairings | A satellite can make ordinary R codec lookup ambiguous or unusable | **Fixed** (#6) |

### P1. Fractional `max_output` and `max_ratio` can silently weaken or disable limits

#### Affected code

- `R/compress.R:152-176`, `zu_check_limit()`
- `R/decompress.R:38-43`, where the validated values are narrowed for
  [`.Call()`](https://rdrr.io/r/base/CallExternal.html)
- `src/zukomp_r.c:211-218`, `zu_int_u64_from_real()`
- `tests/consumer/zukomptest/R/http.R:139-155`, which contains the same
  limit validator pattern for the installed-API consumer

#### Observed behavior

`zu_check_limit()` currently accepts every finite, non-negative numeric
value. It does not require the number to be integral. The value is then
narrowed:

- `max_output` is cast from an R double to `uint64_t` in
  `zu_int_u64_from_real()`; C truncates the fractional part toward zero.
- `max_ratio` is narrowed with
  [`as.integer()`](https://rdrr.io/r/base/integer.html) before entering
  C; R also truncates the fractional part toward zero.
- Native value `0` deliberately means “no limit.” Therefore any
  requested limit in the interval `(0, 1)` becomes `0` and disables the
  limit outright.

Minimal reproduction:

``` r

x <- raw(100000)
z <- komp_compress(x, "gzip")

length(komp_decompress(z, "gzip", max_output = 0.5))
#> [1] 100000

length(komp_decompress(z, "gzip", max_ratio = 0.5))
#> [1] 100000
```

Both calls should reject the nonsensical fractional limit. Instead they
decode the entire payload. Other fractions are silently rounded down as
well: `max_output = 1.5` behaves as a one-byte limit, and
`max_ratio = 1.5` behaves as a ratio of one. This makes the effective
policy different from the policy the caller supplied even when it does
not fully disable the guard.

#### Root cause and risk

The comments above `zu_check_limit()` correctly explain that limits must
be validated before numeric narrowing, but the validator checks range
only. The nearby `zu_check_count()` already applies the missing
invariant with `x != trunc(x)`.

This is security-relevant because these arguments are the
decompression-bomb guards. Configuration often reaches them through
options, environment-derived values or deserialized configuration rather
than integer literals in source. A typo such as `0.5`, or a computed
fractional value, silently converts a restrictive policy into the native
unlimited sentinel.

#### Recommended fix

1.  Reject every finite fractional limit in `zu_check_limit()` before
    either narrowing conversion. Keep `NULL`, explicit `0`, and `Inf` as
    the documented unlimited spellings.

    The central predicate should include the equivalent of:

    ``` r

    (is.finite(x) && x != trunc(x))
    ```

    Update the diagnostic to say that the value must be a single
    non-negative **whole** number, `NULL`, or `Inf`. Do not silently
    round, because neither floor nor ceiling reliably represents the
    caller’s intended security policy.

2.  Add a native backstop to `zu_int_u64_from_real()`. After the
    existing finite/range checks, cast to a temporary `uint64_t` and
    require converting it back to `double` to equal the original value
    before assigning `*out`. R should remain the source of the
    user-facing error, but the C boundary should not accept an invalid
    fractional value from a future or test caller.

3.  Apply the same whole-number validation to the copied consumer helper
    in `tests/consumer/zukomptest/R/http.R`. A reference consumer must
    not teach a weaker validation contract than the package itself.

4.  If fractional ratios are intentionally desired in a future API, make
    that a separate API change: use a floating-point representation
    throughout and define its comparison/rounding semantics. The current
    `uint32_t` ABI cannot preserve a fractional ratio.

#### Required regression tests / acceptance criteria

Add coverage near `tests/testthat/test-arg-narrowing.R` for both
arguments:

- `0.5` and `1.5` are rejected as `zukomp_invalid_argument` before
  native decompression starts. Snapshot the complete error because it is
  part of the R API.
- Whole-number doubles such as `1`, `2`, and their integer equivalents
  retain identical behavior.
- `NULL`, `0`, and `Inf` continue to select the documented unlimited
  behavior.
- A small positive whole limit still raises `zukomp_output_limit` or
  `zukomp_ratio_limit` as applicable.
- Direct test-harness coverage confirms the C double-to-`uint64_t`
  conversion rejects a fractional value, so future R-side regressions
  cannot hide it.
- Consumer-package tests pin the same contract for its limit helper.

This item is closed only when no finite fractional input can reach an
integer limit field through either the package API or the shipped
consumer example.

#### Resolution

Fixed as recommended. `zu_check_limit()` rejects any finite fractional
value; `zu_int_u64_from_real()` carries the native backstop (narrow,
widen back, require equality); the consumer package’s copy of the
validator gained the same rule. `NULL`, `0` and `Inf` remain the
unlimited spellings. Covered in `test-arg-narrowing.R`, including the
exact `max_output = 0.5` reproduction.

Note this is technically a breaking change:
`max_output = object.size(x) * 1.1` used to be accepted. Free before a
release, not after.

### P1. Loading a declared satellite codec does not invalidate a warm R codec cache

#### Affected code

- `R/codecs.R:38-64`, the memoized `zu_codec_table()` closure
- `src/zukomp_r.c:61-83`, `zu_int_codec_rows()` and
  `zukomp_codec_count()`
- R argument validation paths that use the cached table, especially
  `zu_check_codec_name()` before explicit compression/decompression

#### Observed behavior

The cache is invalidated only when the number of displayed table rows
changes:

``` r

n <- .Call(zukomp_codec_count)
if (is.null(cache) || !identical(n, rows)) {
  # rebuild
}
```

`zukomp_codec_count()` returns `zu_int_codec_rows()`: the count of all
declared codec rows plus registrations with an undeclared ID. A
satellite implementation of a declared codec such as `zstd` changes an
existing row from unavailable to available; it does not add a displayed
row. The count therefore remains ten and the cached data frame is not
rebuilt.

Load-order sequence:

1.  [`zukomp::komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
    is called before the satellite DLL loads. The declared `zstd` row is
    cached with `available = FALSE`.
2.  The satellite registers a vtable for `ZU_CODEC_ZSTD` successfully.
3.  The direct native availability query sees the registration, so
    `komp_codec_available("zstd")` can return `TRUE`.
4.  [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
    still returns its stale `available = FALSE` row.
5.  Explicit `komp_compress(..., codec = "zstd")` and
    `komp_decompress(..., codec = "zstd")` validate against that stale
    table and reject the codec as known but not installed.

If the satellite loads before the first table access, the same codec
works. The public behavior therefore depends on package/DLL load order.
Detection via `codec = "auto"` can also follow a different path from
explicit selection, making the inconsistency harder to diagnose.

#### Root cause and risk

The cache key measures presentation shape, not the state it caches. A
registration can alter `available`, capabilities, levels, source and
content encoding without changing the number of rows. The source
comments recognize late satellite loading but assume row-count change is
a sufficient proxy for registry mutation; declared codecs are the
counterexample.

This blocks the primary extension design: separately distributed
implementations of names already declared by zukomp. Applications
commonly enumerate capabilities during startup, before optional packages
load, so this is not an exotic sequence.

#### Recommended fix

Use registry mutation state as the cache key.

Preferred design:

1.  Maintain a monotonic registry generation counter in the native
    registry.
2.  Increment it only after every successful `zu_register_codec()`
    insertion.
3.  Expose a small internal `.Call` probe returning that generation.
4.  Cache both the table and generation in `zu_codec_table()`; rebuild
    whenever the observed generation differs.
5.  Continue using `zu_int_codec_rows()` only to allocate the table
    vectors.

Because registration currently only appends and the registry has a hard
cap, using `zu_int_registry_count()` as the key is also sufficient
today. A named generation is clearer and remains correct if replacement
or deregistration is ever added. Avoid keying on any derived table
property.

The cache closure should conceptually become:

``` r

zu_codec_table <- local({
  cache <- NULL
  generation <- NULL
  function() {
    current <- .Call(zukomp_registry_generation)
    if (is.null(cache) || !identical(current, generation)) {
      cache <<- build_codec_table()
      generation <<- current
    }
    cache
  }
})
```

#### Required regression tests / acceptance criteria

The existing consumer fixture registers its codec when its namespace/DLL
is loaded, so it cannot reproduce “warm cache, then registration” in the
same ordinary package load. Add a second minimal fixture package or run
an isolated R subprocess with a test-only declared-codec satellite:

1.  Load `zukomp` only and call
    [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
    to warm the cache.
2.  Assert the declared fixture codec (preferably `zstd`) is
    unavailable.
3.  Load the fixture, whose DLL registers a vtable for the matching
    declared ID.
4.  Assert `komp_codec_available("zstd")` is `TRUE`.
5.  Assert a fresh
    [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
    row now reports `available = TRUE`, correct capabilities, source and
    levels.
6.  Assert explicit compression/decompression by that name succeeds and
    round-trips.
7.  Repeat with the opposite load order and assert identical public
    behavior.

A small reversible test codec is enough; the test is for registry/cache
semantics, not zstd compatibility. Prefer a separate process/fixture
over a public registration hook because the registry is process-global
and append-only, which can otherwise contaminate unrelated tests or
parallel workers.

#### Resolution

Fixed with the preferred design: `zu_int_registry_generation()` is a
monotonic counter bumped on every successful insertion, and
`zu_codec_table()` keys on it rather than on any derived table property.

The regression test needed one thing the recommendation did not
anticipate. The consumer fixture registers `xor5a` at a *vendor* id,
which adds a row all by itself — so it would invalidate even the old
row-count key and mask the bug entirely. `ZUKOMPTEST_DECLARED_ONLY`
suppresses that registration, leaving only a declared-codec stub whose
registration changes no row count. The subprocess test was confirmed to
fail against the old key before being kept.

### P2. Gzip member detection checks `0x1f` but not the full `0x1f 0x8b` magic

#### Affected code

- `src/codec_deflate.c:502-536`, especially the `ST_MEMBER_END` branch
  at line 528
- Whole-buffer and streaming paths using `ZU_DEC_CONCAT_MEMBERS` and
  `ZU_DEC_REJECT_TRAILING`

#### Observed behavior

After one gzip member finishes, the concatenation state checks only the
first byte of the next input:

``` c
if (buf->src[buf->src_pos] != 0x1F) {
    s->state = ST_DONE;
    return ZU_STREAM_END;
}
zu_int_deflate_reset(s, ZU_LEVEL_DEFAULT);
```

Any tail beginning with `0x1f` is treated as a new gzip member even when
the second byte is not the required `0x8b`. Reproduction:

``` r

a <- komp_compress(charToRaw("ok"), "gzip")
bad_tail <- c(a, as.raw(c(0x1f, 0x00)))

komp_decompress(bad_tail, "gzip")
#> Error of class `zukomp_invalid_data`

zukomp:::zu_test_stream(
  bad_tail, "gzip", "decode",
  reject_trailing = FALSE
)
#> Error of class `zukomp_invalid_data`
```

For comparison, a tail not beginning with `0x1f` is left for the driver,
so the default path reports `zukomp_trailing_bytes` and the opt-out path
returns `charToRaw("ok")`. The same policy should not change based on
the first byte of otherwise arbitrary trailing junk.

#### Root cause and risk

The code intends to distinguish a legal concatenated gzip member from
trailing bytes so the driver can apply the requested trailing-data
policy. A gzip member is identified by the two-byte magic `1f 8b`, not
the first byte alone. Resetting the inflater after only `1f` commits to
parsing a member too early and converts a trailing-data decision into a
malformed-stream decision.

This is a correctness and policy-enforcement defect:

- With trailing rejection enabled, the public error class is wrong.
- With trailing rejection disabled, data the caller explicitly elected
  to ignore can still fail decoding.
- A one-byte input chunk can split the magic, so a naive `src[pos + 1]`
  read is both unsafe and insufficient.

#### Recommended fix

Introduce an explicit “probing next gzip member” state that confirms
both magic bytes before resetting the inflater. It must support `0x1f`
and `0x8b` arriving in separate input buffers.

The implementation must decide the probe without reading past `src_size`
and must preserve these outcomes:

- next bytes are `1f 8b`: reset and parse the next member;
- next available byte is not `1f`, or the byte after `1f` is not `8b`:
  apply trailing-byte policy, not invalid-member policy;
- only `1f` is available and more input may arrive: request input
  without declaring success or truncation;
- end-of-input occurs during a confirmed member: report
  truncation/invalid data according to the existing member parser
  contract.

There is an important cursor-design detail. If the first probe byte is
consumed from one buffer and the second later disproves the magic, the
implementation can no longer make that first byte appear unconsumed
through the current per-buffer `src_pos`. Either:

1.  add a small two-byte lookahead/pushback mechanism in the streaming
    driver so speculative bytes can remain logically trailing, or
2.  let the codec own the probe bytes and return the final
    trailing-policy status itself on mismatch, with explicitly
    documented consumption semantics.

The first approach best preserves exact input-consumption accounting.
The second is smaller but must not silently violate any installed C API
guarantee about the first unconsumed byte. Do not solve this with an
unconditional two-byte peek because the streaming API permits one-byte
chunks.

#### Required regression tests / acceptance criteria

Extend `tests/testthat/test-members.R` and the native/consumer stream
tests:

- Valid concatenated gzip members still decode to the concatenated
  payload.
- Tail `1f 00` produces `zukomp_trailing_bytes` when rejection is
  enabled.
- The same tail is ignored and the first member returned when rejection
  is disabled.
- Run those cases with `in_chunk = 1` and with a chunk large enough to
  contain both probe bytes.
- A complete `1f 8b` prefix followed by an invalid gzip header remains
  invalid data; it is not downgraded to generic trailing junk after the
  magic has been confirmed.
- A valid second member split at every header byte still works.
- Pin the chosen semantics for a final lone `0x1f` byte. It is either a
  truncated possible member or trailing data, but it must be deliberate,
  documented and independent of chunk boundaries.
- If the API promises precise consumption, assert the reported input
  cursor for rejected and tolerated trailing bytes.

#### Resolution

Fixed with approach 2 — the codec owns the probe — after finding
approach 1 unavailable: the driver only refills once the codec has
consumed its input, so returning `ZU_NEED_INPUT` on an unconsumed byte
deadlocks rather than producing a pushback opportunity.

The probe decides without consuming whenever both bytes are visible,
which keeps `src_pos` exact for every case the recommendation listed.
When only `0x1f` is visible and more input may follow, it is consumed
and held in `probe_magic`. The one state `src_pos` cannot then express —
input ending on a held `0x1f` — is where the codec applies
`ZU_DEC_REJECT_TRAILING` itself; without that, the same trailing byte
was reported at a large chunk size and silently swallowed at
`in_chunk = 1`, which is exactly the chunk-dependence the item warned
about.

Semantics for a final lone `0x1f`, as requested: **trailing data**, not
a truncated member, identically at every chunk size. `test-members.R`
pins every tail shape at chunk sizes 1, 2, 3 and 4096.

### P2. `zu_register_codec()` accepts duplicate names and inconsistent codec identities

#### Affected code

- `src/zu_registry.c:84-118`, `zu_register_codec()`
- `src/zu_registry.c:122` onward, name lookup behavior
- `inst/include/zukomp.h`, the public registration and codec-identity
  contract
- `R/codecs.R` and R name validation, which assume one table row per
  codec name

#### Observed behavior

Registration rejects a duplicate numeric codec ID but never compares
names:

``` c
if (zu_int_registry_lookup((zu_codec) vtable->codec) != NULL) {
    return ZU_ERR_INVALID_ARGUMENT;
}
```

A satellite can therefore register a unique vendor ID such as 1024 with
the name `"gzip"`. Registration succeeds even though the built-in gzip
codec already owns that public name. The capability table then contains
two rows whose `id` is `"gzip"`.

R name validation filters the table by `id`; it expects zero or one row.
With two matches, scalar conditions such as `if (!row$available)`
receive a length-two logical vector and error with “the condition has
length \> 1.” All ordinary R operations naming gzip can become unusable
for the rest of that session. Native lookup happens to prefer the
declared built-in due to its search order, so R and C can also resolve
the same name differently.

Related invalid registrations are not ruled out explicitly:

- a declared numeric identity paired with a different name;
- a vendor identity claiming a name reserved by an unavailable declared
  codec such as `zstd`;
- an identity in the reserved numeric gap below `ZU_CODEC_VENDOR_BASE`
  that is neither a known declared enum nor a valid vendor ID;
- potentially duplicate HTTP `content_encoding` tokens, for which lookup
  is likewise first-match wins.

#### Root cause and risk

The registry validates vtable shape, required function pointers,
capacity and numeric-ID uniqueness. It does not validate uniqueness of
every key used to look codecs up, nor the relationship among an ID, its
declared name and the vendor range. Downstream code relies on stronger
invariants than registration actually enforces.

This is especially damaging because registration is process-global and
there is no removal API. One malformed or malicious satellite can poison
codec discovery for the entire R session.

#### Recommended fix

Centralize and enforce registry identity invariants before insertion:

1.  If `vtable->codec` is a declared codec ID, require `vtable->name` to
    exactly match that declaration’s canonical name.
2.  If it is not declared, require
    `vtable->codec >= ZU_CODEC_VENDOR_BASE`. Reject unknown values in
    reserved gaps.
3.  Reject a name equal to any declared codec name unless this vtable
    uses that declaration’s exact numeric ID. This must include
    declared-but-unavailable codecs, which reserve their public names
    for satellite implementations.
4.  Iterate existing registrations and reject any duplicate numeric ID
    or exact canonical name before appending.
5.  Document name comparison semantics. Codec names are currently
    lower-case protocol identifiers; either require canonical lower-case
    at registration or perform the same normalization everywhere.
6.  Decide and document whether two codecs may share a non-empty
    `content_encoding`. Because content-encoding lookup selects one
    codec, the safest invariant is case-insensitive uniqueness unless
    aliases are an intentional, specified feature.
7.  Return `ZU_ERR_INVALID_ARGUMENT` for every identity-contract
    violation and document that status in the installed header.

Keep these checks in the native registration boundary. R-side
deduplication of the table would only hide a corrupted registry and
leave C lookup ambiguous.

#### Required regression tests / acceptance criteria

Use isolated subprocesses or separate fixture packages because
successful registrations cannot be undone in-process. Cover at least:

- vendor ID + built-in name (`1024`, `"gzip"`) is rejected;
- declared ID + wrong name is rejected;
- unknown ID below `ZU_CODEC_VENDOR_BASE` is rejected;
- duplicate registered name with a different vendor ID is rejected;
- duplicate numeric ID remains rejected;
- a valid implementation of a declared codec with its exact ID/name
  succeeds;
- a valid, uniquely named vendor codec at or above the vendor base
  succeeds;
- after every rejected registration, the original codec still resolves
  and operates normally, and
  [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
  still has unique `id` values;
- content-encoding duplicates follow the newly documented rule.

For defense in depth, add an internal assertion or package test that
`anyDuplicated(komp_codecs()$id) == 0L`. The registration boundary
remains the actual fix.

------------------------------------------------------------------------

#### Resolution

Fixed at the registration boundary, as recommended, covering all of
points 1-6: declared ids must carry their declared name; undeclared ids
must sit at or above `ZU_CODEC_VENDOR_BASE`; declared names — *including
declared-but-absent ones* — are reserved for their declared id; and
registered names and content-coding tokens must be unique, the latter
case-insensitively.

Tested from the consumer package, which is the only place real
registrations happen. Rejected registrations mutate nothing, so the
eight invalid shapes run in-process;
`anyDuplicated(komp_codecs()$id) == 0L` is asserted as defence in depth,
as suggested.

## Earlier backlog snapshot

The material below predates the current `main` audit and is retained as
project history. Its original state counts and branch references are no
longer current. Revalidate an item against the present tree before
scheduling it.

**Most of it is now closed.** PR \#6 implemented the abstract level
names (1.1), the decoder-reset coverage (1.2), the flush coverage across
the DEFLATE codecs (1.3), the one-shot compress pair (1.4), `can_flush`
in
[`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
(1.5), the `MZ_ASSERT` guard (2.1), the whole-buffer memory fix (3.1),
the `komp_info()$version` string (4), and `zukomp_invalid_argument` in
design §7. Closed earlier, before that PR: `src/zu_miniz.c` (4),
`_pkgdown.yml` and the site and vignettes (6), the default branch (8),
and `buffer_cap` (2.2). Items 5.1 and 5.4 are stale — all eight
workflows run, and coverage is wired up.

Two diverged from what was recommended, both deliberately:

- **1.1** maps `"fast"`/`"best"` through vtable-declared
  `level_fast`/`level_best`, not through `[level_min, level_max]`.
  DEFLATE’s `level_min` is 0, which is stored blocks, so `"fast"` at the
  range floor returned output *larger* than the input; and an
  acceleration-factor codec such as LZ4 inverts the mapping outright.
  Deriving from the range bakes zlib’s convention into the core.
- **3.1** does *not* pre-size compression from `zu_compress_bound()`.
  The bound is roughly the input size, whereas the growth path — once
  moved to `realloc` — peaks at roughly the *output* size, so pre-sizing
  would lose badly on exactly the compressible data compression is for.
  The external-pointer `realloc` buffer, the item’s other suggestion,
  was done instead and fixes both directions.

Still genuinely open: design §24 criterion 11 (needs a real `zuhttp`),
3.2 benchmarks (phase 2), and 6’s win-builder/macbuilder results for
`cran-comments.md`. Items 5.2 and 5.3 moved to `pedrobtz/r-actions` PR
\#3.

The seven findings from the earlier code-review pass were fixed in
`1568084` and are not repeated below. The remaining notes were
originally ordered by what was expected to hurt soonest.

------------------------------------------------------------------------

## 1. Gaps that will bite `zuhttp` first

These are the parts of the published API that `zuhttp` is documented to
rely on and that nothing currently exercises. None is known to be broken
— that is the problem: nothing would tell us if it were.

### 1.1 The abstract level names from design §4 are not implemented

Design §4 specifies `"fast"`, `"default"` and `"best"` as **“the
portable way to express intent”**, and says codecs without a level axis
accept `"default"`. None of them works:

``` r

komp_compress(x, "gzip", level = "fast")
#> Error: `level` must be a single whole number, or NULL for the codec's default.
```

This is v1 surface that was specified and never built. It matters more
than a convenience usually would, because the design’s whole position on
levels is that **numeric levels are codec-native and not comparable
across codecs** — the strings are the only cross-codec way to say
“compress harder”. A caller writing codec-agnostic code today has no
correct option.

*Do:* implement the three names in `zu_check_level()`, mapping through
each codec’s advertised `[level_min, level_default, level_max]`. Add a
`level = "default"` path for codecs with no level axis. Either that, or
strike the feature from design §4 — but it should not silently stay
unbuilt.

### 1.2 `zu_encoder_reset()` — partly closed, and it was broken

**Closed for the encoder.** `zukomp_test_encoder_reset()` now drives it
and three tests pin the behaviour. Writing them found the bug the item
predicted: `mz_deflateReset()` re-runs `tdefl_init()` with the flags
baked in at `mz_deflateInit2()` time, so a reset with a new level
changed the zlib header’s FLEVEL bits and not the payload. Fixed by
re-initialising miniz when the level actually changes.

**Still open for the decoder.** `zu_decoder_reset()` has no caller
outside its own definition. The state it must get right is larger than
the encoder’s — `total_in`/`total_out` (and so every limit budget), the
wrapper state machine, the gzip header parser and miniz’s own stream.

*Do:* add a reset path to `zu_test_stream()` (e.g. `reset_after`), then
test that a reset stream decodes a second message identically to a fresh
one, that the limit budget restarts rather than carrying over, and that
reset across a *different* codec is refused.

### 1.3 `ZU_FLUSH` is only tested against `identity`

`test-stream.R:45` is the sole `flush_every` test and it uses
`identity`, for which flush is trivially a no-op. The real path —
`MZ_SYNC_FLUSH` through miniz, for gzip and zlib — is untested.

Design §8: *“`ZU_FLUSH` exists because a boolean `finish` cannot express
‘put the bytes on the wire now’ — needed the moment `zuhttp` compresses
a streaming request body.”*

*Do:* extend the flush test across `zlib`, `gzip` and `deflate-raw`.
Assert the useful property: output after intermediate flushes still
decodes to the same input (it will not be byte-identical to unflushed
output, and should not be expected to be).

### 1.4 The one-shot API — partly closed, and it was broken

**Closed for `zu_decompress_one()`.** `zu_test_decompress_one()` drives
it against all four codecs at an exact capacity, one byte short, and at
zero. That found two bugs: a decoder with no output room could never
observe the end of its stream (an empty payload into a zero-byte sink
was reported as an output-limit error), and `mz_inflate()`’s
MZ_FINISH-on-first-call fast path marks a stream permanently failed when
the sink is too small, so an undersized buffer was reported as *corrupt
input*. Both fixed.

**Still open:** `zu_compress_bound()` and `zu_compress_one()` still run
only against `xor5a` in the consumer package — a codec with no wrapper,
no expansion and `bound(n) == n`. `bound()` has never been checked for
gzip or zlib, where it must leave room for a header, a trailer and
stored-block overhead.

*Do:* test the compress pair against all four codecs, including the
incompressible `lcg` payload where `bound()` is closest to being wrong.

### 1.5 `can_flush` is not discoverable from R

The vtable carries `ZU_CAN_FLUSH` and the C ABI exposes it through
`zu_codec_info.flags`, but
[`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
returns only the ten columns design §6 listed, which omit it. An R-level
caller cannot ask whether a codec supports flushing before trying.

*Do:* either add a `can_flush` column (and amend design §6), or document
that flush capability is a C-level concern only.

------------------------------------------------------------------------

## 2. Known hazards, deliberately deferred

### 2.1 `MZ_ASSERT` can abort the R session

miniz has 26 `MZ_ASSERT` call sites reachable from malformed input, and
`MZ_ASSERT` expands to `assert()`. R supplies `-DNDEBUG` so ordinary and
CRAN builds compile them away — but a `-UNDEBUG` build, which is what
`devtools`’ debug install uses, will `abort()` the whole R session
instead of raising a condition.

`MZ_ASSERT` is defined unconditionally in `miniz.h`, so it cannot be
overridden from the command line. Deferred at Stage 13 and still open.

*Do:* extend `tools/patches/miniz/` to make it
`#ifndef MZ_ASSERT`-guarded, then define it to a no-op (or to something
that records a status) in `src/Makevars`. This is the last place in the
package where hostile input can terminate the process rather than return
an error.

### 2.2 `buffer_cap` is a dead parameter — **closed**

Deleted, along with `zu_int_grow()`’s `cap` parameter, which nothing
else set either. `zu_int_reserve()` now asks `zu_int_grow()` for the
actual shortfall rather than for `extra`, so the latent off-by-one is
gone rather than merely harmless. `max_output` in the driver is the only
bound on decompressed size, which is what design §20 intends.

------------------------------------------------------------------------

## 3. Performance

### 3.1 Whole-buffer decompression peaks at ~3× the output size

Measured: decoding 64 MB costs ~199 MB of R peak memory, a 3.1× ratio.

`R_alloc` cannot resize, so `zu_int_reserve()` grows by allocating a new
block and copying — and the old block stays on the `vmax` stack until
the enclosing `.Call` returns. With doubling, several superseded blocks
are live at once.

This is correct and safe, and it is the reason
[`komp_decompress()`](https://pedrobtz.github.io/zukomp/reference/komp_decompress.md)’s
default cap is 1 GiB rather than something larger; but a caller decoding
a large body pays for it. (`zuhttp` should not: it will use the
incremental C path, which reuses one sink.)

*Do:* for **compression**, size the buffer once from
`zu_compress_bound()` and skip growth entirely — the bound is exactly
what it is for. For decompression, consider an external-pointer-owned
`malloc`/`realloc` buffer with a finalizer, which is already the
ownership pattern used for stream handles.

### 3.2 No benchmarks

Design §22 decision 11 says to benchmark against system zlib and
[`memCompress()`](https://rdrr.io/r/base/memCompress.html). Nothing
exists. Phase 2 item 19.

------------------------------------------------------------------------

## 4. Cleanup

- **`src/zu_miniz.c` is Stage 1 scaffolding.**
  `zukomp:::zu_miniz_version()` is fully redundant with
  `komp_info()$vendored`. Delete the file, its `.Call` registration,
  `R/miniz.R`, and repoint
  [`komp_info()`](https://pedrobtz.github.io/zukomp/reference/komp_info.md)
  and `test-abi.R` at a single source.
- **`komp_info()$version` returns a `package_version` object**, not a
  string, which is slightly awkward to
  [`paste()`](https://rdrr.io/r/base/paste.html) into a log line.
  Consider [`as.character()`](https://rdrr.io/r/base/character.html).

------------------------------------------------------------------------

## 5. CI and infrastructure

### 5.1 No workflow has ever run

This is the largest unknown in the whole package. All seven workflows —
`R-CMD-check`, `pkgdown`, `vendor`, `abi`, `consumer`, `fuzz`,
`native-checks` — were written without a single execution, because the
repo had no remote until the first push. Expect first-run failures: YAML
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
ignores them: R’s `etc/Makeconf` assigns `CFLAGS` with `=`, and make
prefers a makefile assignment over the environment. Verified here with a
sentinel `-D` — absent via the environment, present via `~/.R/Makevars`.

That job is very likely building uninstrumented and therefore cannot
fail. Separately it sets `UBSAN_OPTIONS=print_stacktrace=1` without
`halt_on_error=1`, so UBSan findings print to stderr and leave the job
green — and both bugs fuzzing found in this package were UBSan findings
of exactly that kind.

*Do:* in `r-actions`, write the flags to `~/.R/Makevars` (as `lto.yml`
already does correctly), add `halt_on_error=1`, and assert the flags
reached the compiler. Consider adding a `workflow_call` `inputs:` block
so callers can pass env like `ZUKOMP_SLOW_TESTS`.

### 5.3 `native-checks.yaml` carries a job that should be temporary

`sanitizers-exhaustive` exists only because the shared workflow takes no
inputs and does not halt on UBSan. Delete it once 5.2 is fixed — it is
commented to say so.

### 5.4 Coverage is not wired up

`pedrobtz/r-actions` also provides `coverage.yml`; this repo does not
use it. Given how much of the package is C reached through a small R
surface, line coverage would mostly measure the test harness — but
branch coverage of `R/` would still be informative.

------------------------------------------------------------------------

## 6. Documentation and release readiness

- **No `_pkgdown.yml`, and no site.** `DESCRIPTION` used to advertise
  `https://pedrobtz.github.io/zukomp/`, which is a 404: `pkgdown.yaml`
  deploys only on push to `main`, there is no `gh-pages` branch, and all
  the work is on `develop`. CRAN’s incoming check flags a 404 URL, so
  the URL has been **removed from `DESCRIPTION`** for now. Put it back
  once the site actually deploys.
- **No vignette.** For a package whose central claim is an extensibility
  model, “how to write a satellite codec” is the vignette that would
  earn its place — the consumer package is already a worked example.
- **`cran-comments.md` is written** and `.Rbuildignore`d. CRAN incoming
  checks have now been run with network access
  (`_R_CHECK_CRAN_INCOMING_REMOTE_=true`): the only remaining NOTE is
  the expected “New submission”. Before submitting, add win-builder and
  macbuilder results to that file — neither can be run locally.
- **`NEWS.md` is written for 0.1.0** and will need the usual discipline
  from here.

------------------------------------------------------------------------

## 7. Design-document debt

- **Design §24 criterion 11 is open** and cannot be closed here: it
  names `zuhttp`, which is still an empty skeleton.
  `tests/consumer/zukomptest` proves zukomp *supports* incremental
  decoding (5 MB through a reused 4 KiB sink); the criterion itself
  needs a real client. Recorded in both `.agents/` documents.
- **Three design claims were corrected during implementation** and
  should be read as amended, not as originally written: the six-define
  miniz trim (§12), the `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` rationale
  (§12), and the `Imports:`/`importFrom` namespace-loading claim (§15).
- **`zukomp_invalid_argument` is not in design §7’s condition
  hierarchy** but is raised throughout. Add it to the list.

------------------------------------------------------------------------

## 8. Repository

- **The default branch on GitHub is `develop`**, because that was the
  first branch pushed. `main` now exists but points at the initial
  skeleton commit. If the intended convention is `main` as default,
  change it in the repo settings before the PR is merged.
- **No release tag.** `v0.1.0` should be tagged once the first CI run is
  green.
