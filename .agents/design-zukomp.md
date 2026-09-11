# zukomp Design

**Status:** Draft 2 (supersedes the DEFLATE-only draft)
**Package:** `zukomp`
**Purpose:** A small, extensible byte-compression package for R: one codec-neutral API, one vendored codec today (miniz / DEFLATE family), room for more without an ABI break.
**Implementation:** Portable C99, vendored codec sources, no system libraries.
**Consumers:** R users directly; `zuhttp` and future `zu*` packages through a registered C-callable ABI.

---

## 1. What this package is

`zukomp` is a **codec registry with a uniform byte-in/byte-out API**, plus a first codec family implemented on vendored miniz.

The registry — not DEFLATE — is the product. DEFLATE is the first tenant.

```
        R user                      zuhttp / other packages
           |                                  |
   komp_compress()                    zukomp.h + C-callable
   komp_decompress()                          |
   komp_codecs()                              |
           +------------------+---------------+
                              |
                      zukomp core
             (registry, streams, limits, errors)
                              |
       +---------+------------+-----------+---------+
       |         |            |           |         |
   identity  deflate-raw    zlib        gzip     (future: brotli,
                                                  zstd, lz4, snappy)
```

Design consequences, in priority order:

1. **Adding a codec must not change the ABI.** Codecs are enum values behind a vtable, discovered at runtime.
2. **Codecs may live outside this package.** A satellite package can register a codec into the core at load time.
3. **Security properties live in the core, not the codec.** Output caps, ratio caps, and overflow checks are enforced by the stream driver so every codec inherits them.
4. **Nothing codec-specific leaks into a public name.** No `deflate_*` R functions, no `inflater`/`deflater` C types, no miniz types anywhere.

### Non-goals

ZIP/archive manipulation, tar, PNG, encryption, filesystem archive APIs, and a zlib-compatible ABI. A future `zuzip` may depend on `zukomp`; it will not expand this package.

---

## 2. Motivation

`zuhttp` needs `Content-Encoding: gzip` and `deflate` today, and `br` / `zstd` plausibly later. Vendoring codecs into `zuhttp` couples compression to HTTP, blocks reuse, and multiplies the security-update surface. Depending on system zlib adds a build dependency that defeats the point of a self-contained `zu*` family.

A separate foundational package gives: one place to vendor and update codec sources, one place to fuzz, one implementation loaded per R process, and a codec set that other `zu*` packages can query at runtime.

---

## 3. The one-axis codec model

The earlier draft used two axes: an algorithm baked into the function name, and a `format` argument for the wrapper. That does not survive a second algorithm — and the codecs on the roadmap have the same wrapper/no-wrapper split, so the wrapper is not a DEFLATE peculiarity.

**Decision: wrapper variants are distinct codec identities.** One axis, one enum, one argument.

| id | ABI constant | sniffable | HTTP token | notes |
|---|---|---|---|---|
| `identity` | `ZU_CODEC_IDENTITY` | n/a | `identity` | pass-through; simplifies pipelines |
| `deflate-raw` | `ZU_CODEC_DEFLATE_RAW` | no | — | RFC 1951, no header/checksum |
| `zlib` | `ZU_CODEC_ZLIB` | weak | `deflate` | RFC 1950, Adler-32 |
| `gzip` | `ZU_CODEC_GZIP` | `1f 8b` | `gzip` | RFC 1952, CRC-32 + ISIZE |
| `brotli` | `ZU_CODEC_BROTLI` | **no** | `br` | future |
| `zstd` | `ZU_CODEC_ZSTD` | `28 b5 2f fd` | `zstd` | future |
| `lz4-frame` | `ZU_CODEC_LZ4_FRAME` | `04 22 4d 18` | — | future |
| `lz4-block` | `ZU_CODEC_LZ4_BLOCK` | no | — | future; distinct format |
| `snappy-frame` | `ZU_CODEC_SNAPPY_FRAME` | `ff 06 00 00 sNaPpY` | — | future |
| `snappy-raw` | `ZU_CODEC_SNAPPY_RAW` | no | — | future; distinct format |

Two entries deserve emphasis because they shape the API:

- **Brotli has no magic bytes.** It joins raw DEFLATE in the un-sniffable bucket. Any "just detect it" story is wrong for the codec HTTP users will most want detected.
- **`Content-Encoding: deflate` is ambiguous in the wild** (some servers send raw DEFLATE). `zu_codec_from_content_encoding("deflate")` returns `ZU_CODEC_ZLIB`; the retry-as-raw policy belongs in `zuhttp`, not here.

### Codec numbering

```c
typedef enum {
    ZU_CODEC_NONE         = 0,   /* invalid / unknown */
    ZU_CODEC_IDENTITY     = 1,
    ZU_CODEC_DEFLATE_RAW  = 2,
    ZU_CODEC_ZLIB         = 3,
    ZU_CODEC_GZIP         = 4,
    /* 5..15 reserved: DEFLATE family */
    ZU_CODEC_BROTLI       = 16,
    ZU_CODEC_ZSTD         = 17,
    ZU_CODEC_LZ4_FRAME    = 18,
    ZU_CODEC_LZ4_BLOCK    = 19,
    ZU_CODEC_SNAPPY_FRAME = 20,
    ZU_CODEC_SNAPPY_RAW   = 21,
    /* 22..1023 reserved for zukomp */
    ZU_CODEC_VENDOR_BASE  = 1024 /* third-party registrations start here */
} zu_codec;
```

Values are permanent. A codec that is compiled out keeps its number and reports unavailable.

---

## 4. Compression levels

A single 0–9 scale with "conventional zlib expectations" is wrong across codecs: Brotli is 0–11, zstd runs roughly −7…22, LZ4 uses an *acceleration* factor where higher means faster, and Snappy has no level at all.

**Decision:**

- `level = NULL` (R) / `ZU_LEVEL_DEFAULT` (C) means *this codec's default*. This is the documented normal case.
- Integer levels are **codec-native** and validated against the codec's advertised `[level_min, level_max]`. `komp_codecs()` publishes the range.
- The abstract strings `"fast"`, `"default"`, `"best"` map per codec and are the portable way to express intent.
- No cross-codec numeric equivalence is claimed or implied. `level = 6` means different things for gzip and zstd, and that is fine because the codec is always named alongside it.
- Codecs without levels accept `NULL`, `"default"`, and their single valid integer; anything else is `ZU_ERR_INVALID_ARGUMENT`.

---

## 5. Detection (`codec = "auto"`)

Detection is a registry property, not a hardcoded gzip/zlib check.

Each codec may advertise either fixed magic bytes at a fixed offset, or a `sniff` callback for formats whose header is a validity predicate rather than a constant (zlib's CMF/FLG: `CM == 8`, `CINFO <= 7`, `(CMF << 8 | FLG) % 31 == 0`).

Rules:

1. Codecs with fixed magic are tested first, longest magic first.
2. Predicate sniffers (currently only zlib) are tested last, because a weak header check will happily accept arbitrary bytes.
3. If nothing matches: error `zukomp_undetectable_codec`. **`auto` never falls back to a headerless codec.** Raw DEFLATE, raw LZ4 blocks, raw Snappy and Brotli must be requested by name.
4. `zu_sniff()` is exposed so callers can detect without committing to decode.

---

## 6. Public R API

Prefix: **`komp_`**. Distinctive enough to avoid collision if several `zu*` packages are attached at once, short enough to type. (The C ABI uses `zu_`; see §14.)

### v1 surface

```r
komp_compress(x, codec = "gzip", level = NULL)

komp_decompress(
  x,
  codec      = "auto",
  max_output = getOption("zukomp.max_output", 1024^3),
  max_ratio  = getOption("zukomp.max_ratio", NULL)
)

komp_detect(x)        # -> codec id or NA_character_

komp_codecs()         # -> data.frame, the capability table
komp_codec_available(codec)

komp_info()           # package version, vendored source versions, build flags
```

`x` is a raw vector; the return is a raw vector. **Bytes in, bytes out is the whole contract.** Character input is not accepted in v1 — a `komp_compress_text(x, encoding = "UTF-8")` helper can come later without blurring the core.

`komp_codecs()` returns one row per registered codec:

```
id            character   "gzip"
available     logical
can_encode    logical
can_decode    logical
level_min     integer     NA if the codec has no levels
level_max     integer
level_default integer
detectable    logical
content_encoding character  NA if not an HTTP content-coding
source        character   "zukomp" or the registering package
```

This function is the R-visible face of the registry and the reason the package can honestly call itself extensible. It is also the contract `zuhttp` uses to build `Accept-Encoding` — see §16.

### Deferred to phase 2+

R-level streaming objects, file helpers, connection wrappers, dictionaries, `komp_compress_text()`. None of them change the v1 shape.

---

## 7. Error model

C returns status codes; **R constructs conditions**. The C layer never calls `Rf_error()` while holding heap state (§13).

```c
typedef enum {
    ZU_OK = 0,
    ZU_NEED_INPUT,          /* progress possible with more input */
    ZU_NEED_OUTPUT,         /* progress possible with more output room */
    ZU_STREAM_END,

    ZU_ERR_INVALID_ARGUMENT,
    ZU_ERR_UNSUPPORTED,     /* codec not compiled in / not registered */
    ZU_ERR_INVALID_DATA,
    ZU_ERR_TRUNCATED,
    ZU_ERR_CHECKSUM,
    ZU_ERR_TRAILING,
    ZU_ERR_MEMORY,
    ZU_ERR_OUTPUT_LIMIT,
    ZU_ERR_RATIO_LIMIT,
    ZU_ERR_INTERNAL
} zu_status;
```

`ZU_OK` is 0; no negative values are ever returned. `zu_status_string()` covers every enumerator (there is a test that asserts this).

R condition hierarchy:

```
error / zukomp_error
├── zukomp_unsupported_codec
├── zukomp_undetectable_codec
├── zukomp_invalid_data
├── zukomp_truncated
├── zukomp_checksum_error
├── zukomp_trailing_bytes
├── zukomp_output_limit
├── zukomp_ratio_limit
├── zukomp_memory_error
└── zukomp_internal_error
```

Every condition carries `codec`, `input_bytes`, `output_bytes`, and `native_status`. Messages stay one line; the data is for programmatic handling (`zuhttp` will branch on `zukomp_invalid_data` to implement its deflate fallback).

---

## 8. Public C ABI: types

The header installs to `inst/include/zukomp.h`, compiles standalone as C99 against only `<stddef.h>`/`<stdint.h>`, and mentions neither R nor miniz. R-specific resolution lives in a second header, `zukomp-r.h`.

```c
#define ZUKOMP_ABI_VERSION 1
#define ZU_LEVEL_DEFAULT   INT32_MIN

typedef struct {
    const uint8_t *src;  size_t src_size;  size_t src_pos;
    uint8_t       *dst;  size_t dst_size;  size_t dst_pos;
} zu_buffer;
```

**Cursor semantics, stated so they cannot be misread:** the callee advances `src_pos` and `dst_pos` and never touches the other fields; the caller preserves both across calls and only resets them when it swaps a buffer. The earlier `input_used`/`output_used` shape was ambiguous about whether counts were per-call or cumulative, which is exactly the kind of thing that produces a silent data-corruption bug two packages downstream.

```c
typedef enum {
    ZU_RUN    = 0,   /* consume what you can, buffer the rest */
    ZU_FLUSH  = 1,   /* emit everything buffered so far, stream continues */
    ZU_FINISH = 2    /* no more input follows; terminate the stream */
} zu_flush;
```

`ZU_FLUSH` exists because a boolean `finish` cannot express "put the bytes on the wire now" — needed the moment `zuhttp` compresses a streaming request body. Codecs that cannot flush return `ZU_ERR_UNSUPPORTED` for `ZU_FLUSH`; the capability is advertised in the codec info.

```c
typedef struct {
    uint32_t struct_size;      /* sizeof(zu_encoder_opts) at caller compile time */
    zu_codec codec;
    int32_t  level;            /* ZU_LEVEL_DEFAULT */
    uint32_t flags;
} zu_encoder_opts;

typedef struct {
    uint32_t struct_size;
    zu_codec codec;
    uint64_t max_output;       /* 0 = unlimited */
    uint32_t max_ratio;        /* 0 = unlimited */
    uint32_t flags;            /* ZU_DEC_REJECT_TRAILING, ZU_DEC_CONCAT_MEMBERS */
} zu_decoder_opts;
```

The leading `struct_size` is what lets a v1 consumer keep working when v2 appends fields. The earlier draft applied this trick to the API table but passed `int level` as a bare argument — which would have forced an ABI break at the first codec with a differently-shaped parameter.

```c
typedef struct {
    uint32_t    struct_size;
    zu_codec    codec;
    const char *name;
    const char *content_encoding;   /* NULL if not an HTTP content-coding */
    const char *source;             /* package that registered it */
    int32_t     level_min, level_max, level_default;
    uint32_t    flags;              /* ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH */
    int         detectable;
} zu_codec_info;
```

`zu_codec_info` is the read-only projection of a codec's vtable (§10) and the C-level source of everything `komp_codecs()` reports.

Opaque handles are `zu_encoder` / `zu_decoder`. Not `inflater`/`deflater`: that is DEFLATE vocabulary in a codec-neutral header.

---

## 9. Public C ABI: functions

```c
/* discovery ------------------------------------------------------- */
zu_codec    zu_codec_lookup(const char *name);
zu_codec    zu_codec_from_content_encoding(const char *token);
int         zu_codec_available(zu_codec codec);
zu_status   zu_codec_get_info(zu_codec codec, zu_codec_info *out);
zu_status   zu_codec_list(zu_codec *out, size_t cap, size_t *n_out);
zu_status   zu_sniff(const uint8_t *buf, size_t n, zu_codec *out);

/* streaming ------------------------------------------------------- */
zu_status   zu_encoder_new(zu_encoder **out, const zu_encoder_opts *opts);
zu_status   zu_encoder_process(zu_encoder *e, zu_buffer *buf, zu_flush flush);
zu_status   zu_encoder_reset(zu_encoder *e, const zu_encoder_opts *opts);
void        zu_encoder_free(zu_encoder *e);

zu_status   zu_decoder_new(zu_decoder **out, const zu_decoder_opts *opts);
zu_status   zu_decoder_process(zu_decoder *d, zu_buffer *buf, zu_flush flush);
zu_status   zu_decoder_reset(zu_decoder *d, const zu_decoder_opts *opts);
void        zu_decoder_free(zu_decoder *d);

/* one-shot -------------------------------------------------------- */
zu_status   zu_compress_bound(zu_codec c, int32_t level, size_t n, size_t *out);
zu_status   zu_compress_one(const zu_encoder_opts *opts,
                            const uint8_t *src, size_t n,
                            uint8_t *dst, size_t cap, size_t *written);
zu_status   zu_decompress_one(const zu_decoder_opts *opts,
                              const uint8_t *src, size_t n,
                              uint8_t *dst, size_t cap, size_t *written);

/* misc ------------------------------------------------------------ */
const char *zu_status_string(zu_status s);
uint32_t    zu_abi_version(void);
zu_status   zu_register_codec(const zu_codec_vtable *v);
```

Three additions over the previous draft, each with a concrete caller:

- **`zu_*_reset()`** — a keep-alive HTTP client should allocate one decoder per connection, not one per response. Without reset, the stated goal of low allocation overhead is unreachable through this API. A reset re-parameterises the stream it is given: an explicit `level` must take effect on the *payload*, not only on whatever the wrapper advertises, and `ZU_LEVEL_DEFAULT` means "keep this stream's level", not "revert to the codec's". Per-stream counters (`total_in`/`total_out`, and so every limit budget) start again.
- **`zu_compress_bound()` and the one-shot pair** — `zuhttp` should not have to drive a streaming loop to gzip a 200-byte request body.
- **`zu_codec_available()` / `zu_codec_list()`** — the runtime capability query. This is what makes "add Brotli later" a non-event: new enum value, new vtable, **no ABI bump**, and downstream code that already asks what is available picks it up for free.

Every function returns `zu_status`. (The earlier draft had `zu_inflater_new` returning bare `int` while its sibling returned `zu_status`.)

---

## 10. The codec vtable

```c
typedef struct {
    uint32_t     struct_size;
    uint32_t     codec;              /* zu_codec, or >= ZU_CODEC_VENDOR_BASE */
    const char  *name;
    const char  *content_encoding;   /* NULL if not an HTTP content-coding */
    const char  *source;             /* registering package name */

    int32_t      level_min, level_max, level_default;
    uint32_t     flags;              /* ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH */

    const uint8_t *magic;            /* NULL if not sniffable by constant */
    size_t         magic_len;
    size_t         magic_offset;
    int          (*sniff)(const uint8_t *buf, size_t n);  /* optional predicate */

    zu_status (*encoder_new)(void **st, const zu_encoder_opts *opts);
    zu_status (*encoder_process)(void *st, zu_buffer *buf, zu_flush flush);
    zu_status (*encoder_reset)(void *st, const zu_encoder_opts *opts);
    void      (*encoder_free)(void *st);

    zu_status (*decoder_new)(void **st, const zu_decoder_opts *opts);
    zu_status (*decoder_process)(void *st, zu_buffer *buf, zu_flush flush);
    zu_status (*decoder_reset)(void *st, const zu_decoder_opts *opts);
    void      (*decoder_free)(void *st);

    zu_status (*bound)(int32_t level, size_t n, size_t *out);
} zu_codec_vtable;
```

**Limits are not in the vtable.** `max_output` and `max_ratio` are enforced by the core stream driver, which sees every byte through the cursor. A codec implementation cannot forget to enforce them, cannot enforce them inconsistently, and a third-party codec inherits the protection automatically. This is the single most important reason the registry sits above the codecs rather than beside them.

`zu_register_codec()` is **not thread-safe** and may only be called during package initialization (`R_init_*`), before any encoder or decoder exists. Registering a codec id twice is `ZU_ERR_INVALID_ARGUMENT`.

---

## 11. Vendoring and package split

Vendoring five upstreams is a different problem from vendoring one, and the differences are not cosmetic:

- **Snappy upstream is C++**, which conflicts with a pure-C core.
- **Licenses differ**: miniz MIT; Brotli MIT; zstd BSD-3 **dual-licensed GPLv2**; LZ4 BSD-2 for the library, GPLv2 for the programs; Snappy BSD-3. `License:` and `LICENSE.note` must be accurate per vendored source.
- **Source size**: CRAN starts objecting past roughly 5 MB of source tarball, and zstd alone is substantial.

**Decision: core + satellites.**

```
zukomp                 registry, stream driver, limits, errors,
                       identity + deflate-raw + zlib + gzip (miniz)
                       target: < 1 MB of vendored source

zukomp.brotli          LinkingTo: zukomp, Imports: zukomp
zukomp.zstd            each registers its codec in R_init_*
zukomp.lz4             via zu_register_codec()
zukomp.snappy          (may be C++ — the core stays C)
```

Satellites are `Suggests:` of `zukomp`. When `komp_compress(x, "zstd")` finds the codec unregistered, `zukomp` attempts `loadNamespace("zukomp.zstd")`; if that fails it raises `zukomp_unsupported_codec` naming the package to install. Nothing in the core knows what a satellite contains.

This also disposes of the Snappy C++ problem: C++ is confined to one satellite and never reaches the core or `zuhttp`.

The split is a *shipping* decision, not an architectural one. If a codec turns out small enough to fold into the core later, the codec id and vtable are unchanged.

### Update process

Replace the single-purpose `tools/update-miniz` with a manifest-driven `tools/vendor/`:

```
tools/vendor/manifest.tsv   # source, repo, tag, commit, sha256, license, defines, patches
tools/vendor/fetch          # fetch + verify sha256 + extract + record
tools/vendor/verify         # re-verify the tree against the manifest
tools/patches/<source>/     # explicit, reproducible, ideally empty
```

CI rejects any diff under `src/vendor/` that does not update `manifest.tsv`.

---

## 12. Build configuration

`src/Makevars` stays minimal, sets no optimization flags (CRAN forbids), and declares no `SystemRequirements`.

```make
PKG_CPPFLAGS = -I. -Ivendor/miniz \
  -DMINIZ_NO_ARCHIVE_APIS \
  -DMINIZ_NO_ARCHIVE_WRITING_APIS \
  -DMINIZ_NO_STDIO \
  -DMINIZ_NO_TIME \
  -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES \
  -DMINIZ_NO_PNG_APIS
```

`MINIZ_NO_ZLIB_COMPATIBLE_NAMES` is not optional. **Corrected against miniz 3.1.2 at vendoring time:** the zlib-compatible names are no longer `#define`s onto `mz_*` (as miniz 2.x had them) but `static MZ_FORCEINLINE` *functions* named `compress`, `uncompress`, `deflate`, `inflate`, `crc32`, `adler32` and friends, plus `#define`s for `ZLIB_VERSION`, `MAX_WBITS` and `MAX_MEM_LEVEL`. The conclusion is unchanged and if anything stronger: without the flag, every translation unit that includes `miniz.h` acquires file-scope definitions that collide with the zlib R itself links, and the macros leak regardless. The flag stays mandatory; only the mechanism it defuses has changed.

**`MINIZ_NO_PNG_APIS` is ours, not upstream's.** miniz places `tdefl_write_image_to_png_file_in_memory{,_ex}` in the *deflate* section, guarded only by `MINIZ_NO_DEFLATE_APIS` — which zukomp needs. The archive defines therefore do not remove the PNG writer, and it was verified to survive them (`nm` on the built object). Since §24 criterion 14 requires no PNG symbol to be reachable, `tools/patches/miniz/0001-guard-png-writer.patch` adds an opt-out `#ifndef MINIZ_NO_PNG_APIS` guard, written to be upstreamable unchanged. This is one of two patches the vendored tree carries.

**`0002-validate-match-distance.patch` is also ours.** `tinfl` rejected an out-of-range match distance only under `TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF`, because the test it used — `dist > dist_from_out_buf_start` — is only meaningful there: on a wrapping output buffer that offset returns to 0 every 32 KiB, so it records position in the window rather than how much of the window was ever written. `mz_inflate()` sets that flag only for an `MZ_FINISH` on the first call, and §13's own reasoning makes `codec_deflate.c` rewrite a first-call `MZ_FINISH` to `MZ_SYNC_FLUSH` so a too-small output buffer is not misreported as corrupt input — so zukomp took the unchecked path always. A stream claiming a distance past the bytes it had emitted, or a reserved distance code 30/31, therefore read into the dictionary `mz_inflateInit2()` obtains from `malloc` and never clears, and its contents were returned as decompressed data. RFC 1951 §3.2.5 requires rejection, and §24 criterion 5's "never reports success" is violated by returning bytes. The patch tracks bytes emitted since `tinfl_init()` and bounds the distance by `min(that, window)` — what zlib does with `state->whave`. miniz exposes no preset-dictionary API, so history cannot arise any other way and no valid stream is rejected; `test-malformed.R` asserts both directions, the rejection *and* the absence of false rejections past the window edge.

`assert()` is a related hazard rather than a define: miniz's `MZ_ASSERT` expands to `assert`, and miniz calls it on paths reachable from malformed input. R's own `R_XTRA_CPPFLAGS` supplies `-DNDEBUG`, so ordinary and CRAN builds compile it away; a build with `-UNDEBUG` (which `devtools`' debug install uses) does not, and could abort the R session rather than raise a condition. Hardening this belongs to Stage 13 alongside the rest of the abort-path audit.

Exact defines are re-verified on every vendored update; the manifest records the set that was validated, and `tools/vendor/verify` fails if `src/Makevars` and the manifest drift apart in either direction.

Language level: C99 for project code. No C11 atomics, no compiler intrinsics, no architecture-specific assembly, no non-portable thread APIs. Vendored sources may use whatever dialect upstream requires.

---

## 13. Memory, longjmp safety, and interrupts

The previous draft covered ownership but not R's non-local exit, which is the most likely source of real bugs in a package like this.

**Ownership.** The caller owns input and output buffers. `zukomp` owns stream state only. `zu_*_process()` performs no output allocation. Whole-buffer R functions grow their own output buffer using `zu_grow()` with checked `zu_add()`/`zu_mul()`; never a bare `size *= 2`.

**Rules for the R glue, which are binding:**

1. **No `Rf_error()` from inside the codec loop.** The C layer returns `zu_status`; only the outermost `.Call` entry point raises. Anything else leaks whatever the loop had allocated, because `Rf_error()` longjmps past `free()`.
2. **Growing output buffers use `R_alloc`** with `vmaxget`/`vmaxset`, so the R error path reclaims them, or are owned by an external pointer with a registered finalizer.
3. **Stream state crossing a `.Call` boundary lives in an external pointer** with `R_RegisterCFinalizerEx(ptr, fin, TRUE)`. This is the only safe way to expose R-level streams in phase 2, and building the invariant now costs nothing.
4. **`R_CheckUserInterrupt()` is called in the whole-buffer loop** — decompressing a gigabyte must be interruptible — and it longjmps, so it is subject to rules 1–3.

**Allocation of codec state.** Started on the vendored codec's own allocation path. Note that miniz's `tdefl_compressor` is a few hundred KB; storing it inline in the opaque handle is a real memory decision, not a free optimization, and is deferred until benchmarks justify it. No custom allocator callbacks in v1.

---

## 14. Symbol namespacing

| layer | prefix | rationale |
|---|---|---|
| R exports | `komp_` | distinctive across an attached `zu*` family |
| C ABI | `zu_` | short; already namespaced by `zukomp.h` |
| registration / entry points | `zukomp_` | `R_init_zukomp`, `zukomp_api_v1`, `zukomp_get_api` |
| internal only | `zu_int_` | never installed |

The `zud_*` / `ZUD_*` spellings from the previous draft are retired. Include guard is `ZUKOMP_H`.

Never exported under any circumstances: `deflate`, `inflate`, `compress`, `uncompress`, `deflateInit`, `inflateInit`, `crc32`, `adler32`, or anything else that reads as the zlib ABI.

---

## 15. Downstream linkage

`LinkingTo:` supplies headers only; it does not link object code across installed packages. The ABI is therefore delivered through R's registered C-callable mechanism, with a single versioned table so downstream does one lookup instead of one per function.

```c
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;

    zu_codec  (*codec_lookup)(const char *);
    int       (*codec_available)(zu_codec);
    zu_status (*codec_get_info)(zu_codec, zu_codec_info *);
    zu_status (*sniff)(const uint8_t *, size_t, zu_codec *);

    zu_status (*encoder_new)(zu_encoder **, const zu_encoder_opts *);
    zu_status (*encoder_process)(zu_encoder *, zu_buffer *, zu_flush);
    zu_status (*encoder_reset)(zu_encoder *, const zu_encoder_opts *);
    void      (*encoder_free)(zu_encoder *);

    /* decoder quartet, one-shot pair, status_string, register_codec ... */
} zukomp_api_v1;
```

`abi_version` + `struct_size` let v2 append fields safely. Adding a codec appends nothing — it is a new enum value discovered through `codec_available()`.

```c
void attribute_visible R_init_zukomp(DllInfo *dll) {
    R_registerRoutines(dll, NULL, callMethods, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);
    R_RegisterCCallable("zukomp", "zukomp_get_api", (DL_FUNC) zukomp_get_api);
}
```

### The consumer-side gotcha, stated explicitly

`Imports: zukomp` in `DESCRIPTION` does **not** load zukomp's namespace unless the consumer's `NAMESPACE` contains an actual `import()`/`importFrom()` directive. Without it, `R_GetCCallable("zukomp", ...)` called from `R_init_zuhttp` can fail because zukomp's DLL is not loaded yet.

> **Measured at Stage 12, and this claim did not hold.** The consumer package registers its codec from `R_init_zukomptest`, which resolves the API table via `R_GetCCallable`. Removing the `importFrom()` from its `NAMESPACE` and reinstalling did **not** break registration on R 4.5.2: a package listed in `Imports:` has its namespace — and therefore its DLL — loaded first regardless. The `importFrom()` is still required of consumers (§16) and still recommended by *Writing R Extensions*, because leaning on the `DESCRIPTION` field alone is undocumented behaviour, but it is not the load-bearing thing this section claimed. The lazy resolver below is worth keeping on its own merits; it simply is not what rescues this case.

**Therefore the header helper resolves lazily and caches**, rather than resolving at DLL init:

```c
static const zukomp_api_v1 *zukomp_api(void) {
    static const zukomp_api_v1 *cached = NULL;
    if (cached == NULL) {
        const zukomp_api_v1 *(*get)(uint32_t) =
            (const zukomp_api_v1 *(*)(uint32_t))
                R_GetCCallable("zukomp", "zukomp_get_api");
        cached = get(ZUKOMP_ABI_VERSION);
    }
    return cached;
}
```

`zukomp_get_api()` takes the ABI version the consumer was compiled against and returns `NULL` if it cannot satisfy it, so a version mismatch is a clean error rather than a wild call.

Rejected alternative: shipping `inst/lib/libzukomp.a`. It duplicates codec code into every consumer's `.so`, defeats centralized security updates, and adds PIC and library-path handling on three platforms.

---

## 16. `zuhttp` integration contract

```
Imports:   zukomp     # loads the DLL, provides the C-callables
LinkingTo: zukomp     # provides zukomp.h
```

with an `importFrom(zukomp, ...)` directive in `NAMESPACE` (see §15).

Four contract points, in the order `zuhttp` will hit them:

1. **`Accept-Encoding` is derived, not hardcoded.** `zuhttp` builds the header by walking `zu_codec_list()` and taking each codec's `content_encoding` where `ZU_CAN_DECODE` is set. Install `zukomp.brotli` and `br` appears in outgoing requests with no change to `zuhttp`.
2. **Multiple content-codings are legal.** `Content-Encoding: gzip, br` means decode right-to-left. `zuhttp` chains decoders; `zukomp` provides the primitives and a documented chaining recipe, and enforces `max_output` per stage. A cap on the *number* of stages belongs in `zuhttp`.
3. **`deflate` ambiguity is `zuhttp`'s policy.** Try `ZU_CODEC_ZLIB`; on a `zukomp_invalid_data` / `ZU_ERR_INVALID_DATA` failure at the very first byte, optionally retry as `ZU_CODEC_DEFLATE_RAW`. This quirk does not enter the codec package.
4. **Limits are set by `zuhttp`, enforced by `zukomp`.** `zuhttp`'s design already lists `max_decompressed_bytes` and `max_decompression_ratio` as configuration; those map directly to `zu_decoder_opts.max_output` and `.max_ratio`. Ratio capping is therefore a v1 feature here, not a future one.

No miniz symbol, and no vendored-codec symbol of any kind, appears in `zuhttp` source.

---

## 17. DEFLATE family specifics

**gzip wrapper is ours.** Expect miniz to provide no RFC 1952 handling in its codec APIs — its zlib-compatibility layer has no `windowBits`-style gzip mode — so `zu_gzip.c` implements the wrapper and delegates the payload to miniz, using `mz_crc32`/`mz_adler32` for checksums. Verified against the pinned version during vendoring; if a future miniz supplies it, the wrapper is deleted, not worked around.

Decode must handle the full header, not the minimal 10 bytes: `FEXTRA`, `FNAME`, `FCOMMENT`, and `FHCRC` when present. The wrapper is small, separately fuzzed, and the single most likely place for a parser bug.

**Concatenated members: supported in v1.** RFC 1952 permits them and standard tools produce them. Whole-buffer output concatenates member payloads; streaming continues transparently into the next member. Retrofitting this into a finished state machine is worse than building it in, which is why it is not deferred.

**Trailing bytes.** `zu_buffer.src_pos` after `ZU_STREAM_END` tells the caller exactly how much was consumed, so `zuhttp` can distinguish "stream complete" from "stream complete, junk follows". Whole-buffer R functions reject unconsumed trailing bytes with `zukomp_trailing_bytes` unless the format permits them (a following gzip member). `ZU_DEC_REJECT_TRAILING` controls this at the C level.

**Checksums** are verified, always, and produce `ZU_ERR_CHECKSUM` distinctly from `ZU_ERR_INVALID_DATA`. gzip additionally validates ISIZE. Raw DEFLATE has no checksum — this is a documented property of the format, and a reason `auto` will not guess it.

---

## 18. Determinism

Default gzip output uses `mtime = 0`, `OS = 255` (unknown), no filename, no comment, so:

```r
identical(komp_compress(x, "gzip"), komp_compress(x, "gzip"))
```

holds, and holds across platforms.

**The guarantee is scoped to a fixed `zukomp` version.** Compressed bytes may change when a vendored codec is updated. Deterministic output is for reproducible builds, stable test fixtures, and cache *validation* — it is **not** a content hash, and the documentation says so in those words. The previous draft advertised "hashes" as a use case without this caveat.

---

## 19. Thread safety

- Distinct encoders/decoders are usable concurrently on distinct threads. One stream object, one thread at a time.
- No global mutable state except the codec registry, which is written only during package initialization and read-only thereafter.
- Global tables are immutable and statically initialized.
- The C ABI depends on R only for C-callable resolution, which happens once on the R main thread.
- The R-level API is single-threaded, as all of R is.

---

## 20. Security model

Compressed input is untrusted input; `zuhttp` will feed this package arbitrary bytes from the internet.

- Validate every wrapper header before acting on it.
- Verify every available checksum.
- Check every integer conversion and every buffer arithmetic operation (`zu_add`, `zu_mul`, `zu_grow`).
- Never allocate based on a size claimed by the input. gzip's ISIZE is *validated against* actual output, never used to size a buffer.
- Enforce `max_output` and `max_ratio` in the core driver so no codec can bypass them.
- No recursive parsing anywhere.
- Continuous fuzzing with ASan/UBSan; MSan where practical.
- Track upstream security advisories for every vendored source listed in `manifest.tsv`.

`max_ratio` is off by default: legitimate highly-compressible data routinely exceeds any safe-looking threshold. `zuhttp` sets its own.

---

## 21. Portability

Builds with the ordinary R toolchain on Windows (Rtools/MinGW), macOS, Linux, and other Unix where R builds. No configure-time external dependency, no CMake, no Meson, no system zlib. 32- and 64-bit, little- and big-endian. Header fields are always assembled with explicit byte operations; host-endian integers are never memcpy'd into a wrapper header.

---

## 22. Decision log

Resolving the previous draft's fifteen open questions, so implementation is not blocked on debate:

| # | Question | Decision |
|---|---|---|
| 1 | Which miniz release? | Latest 3.x stable release, pinned by commit + sha256 in `manifest.tsv`. **Settled: 3.1.2**, commit `77d0dce`, internal `MZ_VERSION` 11.3.2. |
| 2 | Does miniz cover the gzip wrapper? | Assume **no**; `zu_gzip.c` owns RFC 1952. **Verified at vendoring time against 3.1.2: confirmed no.** `mz_deflateInit2`/`mz_inflateInit2` accept only `±MZ_DEFAULT_WINDOW_BITS` (zlib or raw); there is no `+16` gzip mode, and gzip appears nowhere in the codec APIs. |
| 3 | Smallest safe define set? | **Six**, not five — the archive/stdio/time/zlib-names four, plus the patched `MINIZ_NO_PNG_APIS` (§12); re-validated per update. |
| 4 | Concatenated gzip members in v1? | **Yes.** |
| 5 | Should `auto` try raw DEFLATE? | **No**, ever. Magic/predicate sniff only; error otherwise. |
| 6 | C symbol prefix? | `zu_` ABI, `zukomp_` entry points, `komp_` R exports. `zud_*` retired. |
| 7 | Default compress codec? | `gzip` — interoperates with everything. |
| 8 | Default `max_output`? | Finite: 1 GiB, overridable via `options(zukomp.max_output=)`; `Inf` is opt-in. |
| 9 | Deterministic gzip mandatory? | Default on, configurable via encoder flags. |
| 10 | R stream objects in v1? | No — C streaming only; R streams in phase 2. |
| 11 | Benchmark against system zlib? | Yes, benchmark only. Never a build dependency. |
| 12 | `tinfl`/`tdefl` direct, or the zlib-compat layer? | Start on the compat layer for correctness; the vtable makes swapping contained. Revisit with benchmarks. |
| 13 | gzip header CRC? | Parse and verify `FHCRC` when the flag is set. |
| 14 | Trailing bytes exposure? | `src_pos` + `ZU_DEC_REJECT_TRAILING` + `zukomp_trailing_bytes` (§17). |
| 15 | Expose version/feature info? | Yes — `komp_info()` and `komp_codecs()`; the latter is the capability contract. |

New decisions this draft adds: the one-axis codec model (§3), codec-native levels (§4), core-enforced limits (§10), the core/satellite split (§11), cursor-style buffers and the flush enum (§8), lazy ABI resolution (§15), and the longjmp rules (§13).

---

## 23. MVP

```
registry + identity codec
vendored miniz (deflate-raw, zlib, gzip)
core stream driver with max_output / max_ratio
gzip wrapper incl. concatenated members and full header
komp_compress / komp_decompress / komp_detect / komp_codecs / komp_info
structured R conditions
registered C-callable versioned API table
external codec registration proven by a test consumer package
test suite per the roadmap, fuzz harness, sanitizer CI
```

Deferred: R streaming objects, file helpers, connection wrappers, `komp_compress_text()`, dictionaries, real satellite codec packages.

---

## 24. Acceptance criteria

1. Builds from source on Windows, macOS, and Linux with no system compression library.
2. `identity`, `deflate-raw`, `zlib`, and `gzip` round-trip across the payload corpus at every valid level.
3. Output is accepted by external decoders, and external encoders' output is accepted here (fixture-based, offline).
4. Streaming is correct for arbitrary 1-byte input and output chunk boundaries, in both directions.
5. Every truncation position of every representative stream errors; none reports success.
6. Every checksum corruption is detected as `zukomp_checksum_error`.
7. `max_output` and `max_ratio` stop decompression deterministically, with the correct condition class.
8. Fuzzing under ASan/UBSan finds no memory-safety failure.
9. No vendored-codec type or symbol appears in `zukomp.h`, and a test asserts it.
10. A separate package consumes the ABI via `Imports` + `LinkingTo` **and registers its own codec**, proving extensibility rather than asserting it.
11. `zuhttp` decodes gzip and deflate responses incrementally, without materializing whole compressed bodies. **[open at v1: `zuhttp` does not exist yet. `tests/consumer/zukomptest` proves zukomp supports it — 5 MB decoded through a reused 4 KiB sink via `zu_decoder_process()` — but the criterion names zuhttp and only zuhttp can close it.]**
12. Adding a codec requires no change to `zukomp.h`'s existing declarations and no ABI bump.
13. Vendored provenance is reproducible from `manifest.tsv` alone.
14. No archive, ZIP, or PNG symbol is reachable, verified by a symbol-audit test.

---

## 25. Positioning

> A small, portable, extensible compression package for R. One API over many codecs, vendored so there is no system dependency, with a stable C ABI other packages can build on.

It competes on dependency surface, portability, streaming correctness, predictable limits and errors, and downstream C reuse — not on beating tuned system zlib at throughput.

**`zukomp` is a codec registry that ships with DEFLATE, not a DEFLATE package with room for extras.**
