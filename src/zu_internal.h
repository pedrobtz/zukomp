/* Internal declarations shared between zukomp's own translation units.
   Never installed, never seen by a consumer: everything here is prefixed
   zu_int_ per design 14 and may change without an ABI bump. */
#ifndef ZU_INTERNAL_H
#define ZU_INTERNAL_H

#include "zukomp.h"

/* A codec this build knows the *name* of, whether or not an implementation
   is registered. Declaring names independently of implementations is what
   lets komp_codecs() advertise "zstd exists, install zukomp.zstd" instead of
   pretending the codec does not exist. */
typedef struct {
    zu_codec    codec;
    const char *name;
    const char *content_encoding;  /* NULL if not an HTTP content-coding */
} zu_int_codec_decl;

size_t                   zu_int_declared_count(void);
const zu_int_codec_decl *zu_int_declared_at(size_t i);
const zu_int_codec_decl *zu_int_declared_for(zu_codec codec);

/* Registered vtable for a codec, or NULL. */
const zu_codec_vtable *zu_int_registry_lookup(zu_codec codec);

/* Registered vtables in registration order, for building the R-side table. */
size_t                 zu_int_registry_count(void);
const zu_codec_vtable *zu_int_registry_at(size_t i);

/* Resolve the abstract level names against a vtable, tolerating one that
   predates the fields (see zu_registry.c). */
/* Monotonic count of successful codec registrations. The R codec-table
   cache keys on this: registry mutation is the state being cached, and the
   displayed row count is not a function of it (a satellite implementing a
   declared codec changes no row count). */
uint64_t zu_int_registry_generation(void);

int32_t zu_int_vtable_level_fast(const zu_codec_vtable *v);
int32_t zu_int_vtable_level_best(const zu_codec_vtable *v);

/* Registers everything zukomp itself implements. Called once from
   R_init_zukomp, before any encoder or decoder can exist. */
zu_status zu_int_register_builtin_codecs(void);

/* Built-in codec vtables. */
extern const zu_codec_vtable zu_int_codec_identity;
extern const zu_codec_vtable zu_int_codec_deflate_raw;
extern const zu_codec_vtable zu_int_codec_zlib;
extern const zu_codec_vtable zu_int_codec_gzip;
extern const zu_codec_vtable zu_int_codec_zstd;
extern const zu_codec_vtable zu_int_codec_lz4_frame;
extern const zu_codec_vtable zu_int_codec_lz4_block;

/* -- frame continuation, for zstd and lz4-frame (src/zu_frames.c) --------
 *
 * Both formats are a sequence of frames: RFC 8878 section 3 and the LZ4
 * frame format both let a stream hold several, and skippable frames
 * (magic 0x184D2A50..5F) between them, and both reference tools decode the
 * concatenation. That is gzip's members over again (design 17), so it takes
 * gzip's policy: ZU_DEC_CONCAT_MEMBERS continues into a following frame,
 * anything that is not a frame magic is left unconsumed for the driver's
 * trailing-bytes policy, and the decision is made without consuming
 * whenever the four magic bytes are all visible. */
typedef enum {
    ZU_INT_FRAME_BODY = 0,   /* inside a frame; the codec's library decides */
    ZU_INT_FRAME_BETWEEN,    /* a frame ended; is another one coming? */
    ZU_INT_FRAME_DONE
} zu_int_frame_state;

typedef enum {
    ZU_INT_FRAMES_ZSTD = 0,
    ZU_INT_FRAMES_LZ4
} zu_int_frames_kind;

typedef struct {
    zu_int_frame_state state;
    zu_int_frames_kind kind;
    uint32_t           flags;     /* ZU_DEC_* from the decoder options */
    uint8_t            held[4];   /* magic bytes consumed while undecided */
    size_t             n_held;
    /* One decoded byte taken from the library by zu_int_frames_stalled()'s
       probe, owed to the caller's next output window. */
    uint8_t            owed;
    int                has_owed;
} zu_int_frames;

/* Hands the codec's library bytes the prober has already consumed, once
   they are confirmed to start a frame. */
typedef zu_status (*zu_int_frames_feed)(void *ctx, const uint8_t *p, size_t n);

void      zu_int_frames_init(zu_int_frames *f, zu_int_frames_kind kind,
                             uint32_t flags);
void      zu_int_frames_ended(zu_int_frames *f);
/* Call while f->state != ZU_INT_FRAME_BODY. ZU_OK means a new frame begins
   and the codec should resume decoding; anything else is the status to
   return from process(). */
zu_status zu_int_frames_between(zu_int_frames *f, zu_buffer *buf,
                                zu_flush flush, zu_int_frames_feed feed,
                                void *ctx);
/* Why a framed decoder that moved nothing stopped, from what it was
   offered. `probe` asks the library for one byte into a scratch buffer and
   reports how many it wrote and whether the frame ended; it is used only in
   the one case the offer cannot settle. See zu_frames.c. */
typedef zu_status (*zu_int_frames_probe)(void *ctx, uint8_t *one,
                                         size_t *written, int *frame_end);
zu_status zu_int_frames_stalled(zu_int_frames *f, size_t avail_in,
                                size_t avail_out, zu_flush flush,
                                zu_int_frames_probe probe, void *ctx);
/* Delivers an owed byte. Non-zero when the codec may carry on. */
int       zu_int_frames_pay(zu_int_frames *f, zu_buffer *buf);

/* -- checked size arithmetic (src/zu_buf.c) ------------------------------
 *
 * Named zu_int_* per design 14: these are internal only and are deliberately
 * absent from zukomp.h, so no consumer can come to depend on them. (The
 * roadmap sketches them as zu_add/zu_mul/zu_grow; the zu_ prefix is reserved
 * for the installed ABI.) */

/* Smallest buffer zu_int_grow will hand back. Avoids a pathological ramp of
   1, 2, 4, 8 ... allocations when a stream starts by producing a few bytes. */
#define ZU_INT_MIN_BUFFER 4096

/* base + off, but yielding NULL instead of undefined behaviour when base is
   NULL. An empty zu_buffer legitimately has a NULL pointer and a zero size,
   and `NULL + 0` is undefined in C even though every real compiler produces
   NULL -- UBSan flags it, and it is trivially avoidable. Found by fuzzing;
   reachable from komp_decompress(raw(0), "deflate-raw"). */
static inline const uint8_t *zu_int_cat(const uint8_t *base, size_t off)
{
    return (base == NULL) ? NULL : base + off;
}

static inline uint8_t *zu_int_at(uint8_t *base, size_t off)
{
    return (base == NULL) ? NULL : base + off;
}

zu_status zu_int_add(size_t a, size_t b, size_t *out);
zu_status zu_int_mul(size_t a, size_t b, size_t *out);
zu_status zu_int_grow(size_t current, size_t needed, size_t *out);

/* -- whole-buffer drive loop (src/zu_whole.c) ----------------------------
 *
 * One loop, shared by komp_compress(), komp_decompress() and the
 * zu_test_stream() harness, so the chunk-boundary sweeps exercise the same
 * code the users run. */

/* The growing output sink.
 *
 * `buf` is malloc'd and grown with realloc, and is owned for its whole life
 * by an R external pointer with a finalizer -- `owner`, kept as void* so
 * this header stays free of R. R_alloc cannot resize, so the previous
 * version grew by allocating a new block and copying, and every superseded
 * block stayed on the vmax stack until the enclosing .Call returned: with
 * doubling, several were live at once and decoding 64 MB peaked at ~199 MB.
 * realloc releases the old block immediately, and the external pointer is
 * what keeps that safe across the interrupt longjmp in the drive loop. */
typedef struct {
    void    *owner;    /* SEXP external pointer owning buf; NULL until made */
    uint8_t *buf;
    size_t   size;
    size_t   used;
    size_t   consumed; /* input bytes the codec actually took */
} zu_int_outbuf;

typedef struct {
    const uint8_t *src;
    size_t         n;
    int            encode;
    zu_codec       codec;
    int32_t        level;
    uint64_t       max_output;
    uint32_t       max_ratio;
    uint32_t       dec_flags;     /* ZU_DEC_EXPECTED_SIZE among them */
    uint64_t       expected_size; /* read only with ZU_DEC_EXPECTED_SIZE */
    size_t         in_chunk;
    size_t         out_chunk;
    uint64_t       flush_every;   /* 0 = never */
} zu_int_run_opts;

zu_status zu_int_run_whole(const zu_int_run_opts *r, zu_int_outbuf *out);

/* NOTE: this header must stay free of R. The core -- registry, driver,
   buffers, codecs, gzip parser -- is compiled without R at all by the fuzz
   targets in fuzz/, which is only possible while nothing here needs
   Rinternals.h. R-dependent internals live in src/zu_rglue.h. */

/* -- gzip wrapper (src/zu_gzip.c) ---------------------------------------- */

#define ZU_INT_GZIP_HEADER_LEN 10

/* FNAME and FCOMMENT are NUL-terminated and unbounded in RFC 1952. We do not
   store them, so the only risk is spending forever on a hostile stream;
   this bound turns that into a clean error. */
#define ZU_INT_GZIP_MAX_FIELD 65535

typedef enum {
    ZU_INT_GZ_FIXED = 0,
    ZU_INT_GZ_EXTRA_LEN,
    ZU_INT_GZ_EXTRA,
    ZU_INT_GZ_NAME,
    ZU_INT_GZ_COMMENT,
    ZU_INT_GZ_HCRC,
    ZU_INT_GZ_DONE
} zu_int_gzip_state;

typedef struct {
    zu_int_gzip_state state;
    uint8_t  fixed[ZU_INT_GZIP_HEADER_LEN];
    size_t   fixed_pos;
    uint8_t  flg;
    uint16_t xlen;
    size_t   xlen_pos;
    size_t   xpos;
    size_t   field_len;
    uint8_t  hcrc[2];
    size_t   hcrc_pos;
    unsigned long crc;      /* over the header, for FHCRC */
} zu_int_gzip_header;

void      zu_int_gzip_write_header(uint8_t out[ZU_INT_GZIP_HEADER_LEN]);
void      zu_int_gzip_header_init(zu_int_gzip_header *h);
zu_status zu_int_gzip_header_feed(zu_int_gzip_header *h, uint8_t byte, int *done);

#endif /* ZU_INTERNAL_H */
