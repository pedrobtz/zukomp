/* Internal declarations shared between zukomp's own translation units.
   Never installed, never seen by a consumer: everything here is prefixed
   zu_int_ per design 14 and may change without an ABI bump. */
#ifndef ZU_INTERNAL_H
#define ZU_INTERNAL_H

#include <Rinternals.h>

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

/* Registers everything zukomp itself implements. Called once from
   R_init_zukomp, before any encoder or decoder can exist. */
zu_status zu_int_register_builtin_codecs(void);

/* Built-in codec vtables. */
extern const zu_codec_vtable zu_int_codec_identity;
extern const zu_codec_vtable zu_int_codec_deflate_raw;
extern const zu_codec_vtable zu_int_codec_zlib;
extern const zu_codec_vtable zu_int_codec_gzip;

/* -- checked size arithmetic (src/zu_buf.c) ------------------------------
 *
 * Named zu_int_* per design 14: these are internal only and are deliberately
 * absent from zukomp.h, so no consumer can come to depend on them. (The
 * roadmap sketches them as zu_add/zu_mul/zu_grow; the zu_ prefix is reserved
 * for the installed ABI.) */

/* Smallest buffer zu_int_grow will hand back. Avoids a pathological ramp of
   1, 2, 4, 8 ... allocations when a stream starts by producing a few bytes. */
#define ZU_INT_MIN_BUFFER 4096

zu_status zu_int_add(size_t a, size_t b, size_t *out);
zu_status zu_int_mul(size_t a, size_t b, size_t *out);
zu_status zu_int_grow(size_t current, size_t needed, size_t cap, size_t *out);

/* -- whole-buffer drive loop (src/zu_whole.c) ----------------------------
 *
 * One loop, shared by komp_compress(), komp_decompress() and the
 * zu_test_stream() harness, so the chunk-boundary sweeps exercise the same
 * code the users run. */

typedef struct {
    char    *vmax;     /* vmaxget() at the start, vmaxset() when done */
    uint8_t *buf;
    size_t   size;
    size_t   used;
} zu_int_outbuf;

typedef struct {
    const uint8_t *src;
    size_t         n;
    int            encode;
    zu_codec       codec;
    int32_t        level;
    uint64_t       max_output;
    uint32_t       max_ratio;
    uint32_t       dec_flags;
    size_t         in_chunk;
    size_t         out_chunk;
    uint64_t       flush_every;   /* 0 = never */
    size_t         buffer_cap;    /* 0 = unlimited */
} zu_int_run_opts;

zu_status zu_int_run_whole(const zu_int_run_opts *r, zu_int_outbuf *out);

/* Packs a native status and the bytes produced so far into the pair every
   R-visible entry point returns, so R -- never C -- decides what is a
   condition (design 13 rule 1). Defined in src/zukomp_test.c. */
SEXP zu_int_result(zu_status status, const uint8_t *bytes, size_t n);

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
