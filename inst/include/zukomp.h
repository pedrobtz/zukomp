/* zukomp -- a codec registry with a uniform byte-in/byte-out API.
 *
 * This is the public C ABI. It compiles standalone as C99 against only
 * <stddef.h> and <stdint.h>: it must mention neither R nor any vendored
 * codec, and no codec-specific vocabulary may appear in a public name.
 * R-specific resolution lives in zukomp-r.h.
 *
 * Copyright (c) 2026 zukomp authors. MIT licensed; see LICENSE.
 */
#ifndef ZUKOMP_H
#define ZUKOMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped only when an existing declaration changes meaning. Adding a codec
   is not an ABI change: codecs are discovered at runtime through the
   registry, so a new codec appends nothing to this header. */
#define ZUKOMP_ABI_VERSION 1

/* -- status ------------------------------------------------------------- */

/* ZU_OK is 0 and no negative value is ever returned, so `if (st)` reliably
   means "not plain success". The first four are progress reports, not
   failures; everything from ZU_ERR_INVALID_ARGUMENT on is an error. */
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

/* -- codec identity ----------------------------------------------------- */

/* Wrapper variants are distinct codec identities, not a format argument on a
   shared algorithm: one axis, one enum. Values are permanent. A codec that
   is not compiled in keeps its number and reports unavailable. */
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

/* -- buffers ------------------------------------------------------------ */

/* Cursor semantics, stated so they cannot be misread: the callee advances
   src_pos and dst_pos and never touches the other fields; the caller
   preserves both across calls and resets them only when it swaps a buffer.
   Counts are therefore cumulative for the life of the buffer, not per-call. */
typedef struct {
    const uint8_t *src;
    size_t         src_size;
    size_t         src_pos;
    uint8_t       *dst;
    size_t         dst_size;
    size_t         dst_pos;
} zu_buffer;

/* ZU_FLUSH exists because a boolean "finish" cannot express "put the bytes
   on the wire now", which a streaming request body needs. Codecs that cannot
   flush return ZU_ERR_UNSUPPORTED for it and do not advertise ZU_CAN_FLUSH. */
typedef enum {
    ZU_RUN    = 0,   /* consume what you can, buffer the rest */
    ZU_FLUSH  = 1,   /* emit everything buffered so far, stream continues */
    ZU_FINISH = 2    /* no more input follows; terminate the stream */
} zu_flush;

/* -- options ------------------------------------------------------------ */

/* Levels are codec-native and validated against the codec's advertised
   range; no cross-codec numeric equivalence is claimed. ZU_LEVEL_DEFAULT
   means "this codec's default" and is the normal case. */
#define ZU_LEVEL_DEFAULT INT32_MIN

/* Decoder option flags. */
#define ZU_DEC_REJECT_TRAILING  0x00000001u  /* error on bytes after the stream */
#define ZU_DEC_CONCAT_MEMBERS   0x00000002u  /* continue into a following member */

/* The leading struct_size is what lets a consumer built against v1 keep
   working when v2 appends fields: set it to sizeof(the struct) as you
   compiled it, and the callee reads only what both sides agree exists. */
typedef struct {
    uint32_t struct_size;
    zu_codec codec;
    int32_t  level;
    uint32_t flags;
} zu_encoder_opts;

typedef struct {
    uint32_t struct_size;
    zu_codec codec;
    uint64_t max_output;   /* 0 = unlimited */
    uint32_t max_ratio;    /* 0 = unlimited */
    uint32_t flags;
} zu_decoder_opts;

/* -- capability description --------------------------------------------- */

/* Codec capability flags, as reported in zu_codec_info.flags. */
#define ZU_CAN_ENCODE  0x00000001u
#define ZU_CAN_DECODE  0x00000002u
#define ZU_CAN_FLUSH   0x00000004u

/* Read-only projection of a codec's vtable, and the C-level source of
   everything the R-side capability table reports. level_* are 0 for a codec
   that has no levels; check the codec's flags rather than inferring. */
typedef struct {
    uint32_t    struct_size;
    zu_codec    codec;
    const char *name;
    const char *content_encoding;  /* NULL if not an HTTP content-coding */
    const char *source;            /* package that registered it */
    int32_t     level_min;
    int32_t     level_max;
    int32_t     level_default;
    uint32_t    flags;
    int         detectable;
} zu_codec_info;

/* -- opaque stream handles ---------------------------------------------- */

/* Deliberately not "inflater"/"deflater": that is DEFLATE vocabulary in a
   codec-neutral header. */
typedef struct zu_encoder zu_encoder;
typedef struct zu_decoder zu_decoder;

/* -- functions ---------------------------------------------------------- */

/* The ABI version this library implements. A consumer compares it against
   the ZUKOMP_ABI_VERSION it was compiled with. */
uint32_t zu_abi_version(void);

/* A short, stable, English description of any status. Never returns NULL,
   and covers every enumerator; there is a test that asserts this. */
const char *zu_status_string(zu_status status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZUKOMP_H */
