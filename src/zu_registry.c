/* The codec registry: a fixed-capacity table of vtable pointers, plus the
   static table of codec names this build knows about.
 *
 * The registry is the only mutable global state in the package. It is
 * written during R_init_zukomp and read-only afterwards, which is what makes
 * the rest of zukomp thread-safe and what lets the test suite run in
 * parallel. Nothing here allocates. */
#include <string.h>

#include "zu_internal.h"

/* -- declared codec names ------------------------------------------------ */

/* Values are permanent (design 3). A codec that is not compiled in keeps its
   number and its name, and reports unavailable. */
static const zu_int_codec_decl zu_int_declared[] = {
    { ZU_CODEC_IDENTITY,     "identity",     "identity" },
    { ZU_CODEC_DEFLATE_RAW,  "deflate-raw",  NULL       },
    { ZU_CODEC_ZLIB,         "zlib",         "deflate"  },
    { ZU_CODEC_GZIP,         "gzip",         "gzip"     },
    { ZU_CODEC_BROTLI,       "brotli",       "br"       },
    { ZU_CODEC_ZSTD,         "zstd",         "zstd"     },
    { ZU_CODEC_LZ4_FRAME,    "lz4-frame",    NULL       },
    { ZU_CODEC_LZ4_BLOCK,    "lz4-block",    NULL       },
    { ZU_CODEC_SNAPPY_FRAME, "snappy-frame", NULL       },
    { ZU_CODEC_SNAPPY_RAW,   "snappy-raw",   NULL       }
};

#define ZU_INT_N_DECLARED \
    (sizeof(zu_int_declared) / sizeof(zu_int_declared[0]))

size_t zu_int_declared_count(void)
{
    return ZU_INT_N_DECLARED;
}

const zu_int_codec_decl *zu_int_declared_at(size_t i)
{
    return (i < ZU_INT_N_DECLARED) ? &zu_int_declared[i] : NULL;
}

const zu_int_codec_decl *zu_int_declared_for(zu_codec codec)
{
    for (size_t i = 0; i < ZU_INT_N_DECLARED; i++) {
        if (zu_int_declared[i].codec == codec) {
            return &zu_int_declared[i];
        }
    }
    return NULL;
}

/* -- the registry -------------------------------------------------------- */

/* Room for every declared codec plus a healthy margin of satellite
   registrations. Overflowing this is ZU_ERR_MEMORY, not a buffer overrun. */
#define ZU_INT_REGISTRY_CAP 32

static const zu_codec_vtable *zu_int_registry[ZU_INT_REGISTRY_CAP];
static size_t zu_int_registry_n = 0;

size_t zu_int_registry_count(void)
{
    return zu_int_registry_n;
}

const zu_codec_vtable *zu_int_registry_at(size_t i)
{
    return (i < zu_int_registry_n) ? zu_int_registry[i] : NULL;
}

const zu_codec_vtable *zu_int_registry_lookup(zu_codec codec)
{
    if (codec == ZU_CODEC_NONE) {
        return NULL;
    }
    for (size_t i = 0; i < zu_int_registry_n; i++) {
        if (zu_int_registry[i]->codec == (uint32_t) codec) {
            return zu_int_registry[i];
        }
    }
    return NULL;
}

zu_status zu_register_codec(const zu_codec_vtable *vtable)
{
    if (vtable == NULL || vtable->name == NULL || vtable->source == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    /* A vtable from a consumer compiled against a newer header may be
       larger than ours; one smaller than ours is missing fields we read. */
    if (vtable->struct_size < sizeof(zu_codec_vtable)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (vtable->codec == (uint32_t) ZU_CODEC_NONE) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    /* Encoding without an encoder, or decoding without a decoder, would
       fail later at a point where the codec is no longer obviously to
       blame. Reject it at registration instead. */
    if ((vtable->flags & ZU_CAN_ENCODE) &&
        (vtable->encoder_new == NULL || vtable->encoder_process == NULL ||
         vtable->encoder_free == NULL)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if ((vtable->flags & ZU_CAN_DECODE) &&
        (vtable->decoder_new == NULL || vtable->decoder_process == NULL ||
         vtable->decoder_free == NULL)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (zu_int_registry_lookup((zu_codec) vtable->codec) != NULL) {
        return ZU_ERR_INVALID_ARGUMENT;   /* already registered */
    }
    if (zu_int_registry_n >= ZU_INT_REGISTRY_CAP) {
        return ZU_ERR_MEMORY;
    }
    zu_int_registry[zu_int_registry_n++] = vtable;
    return ZU_OK;
}

/* -- lookup -------------------------------------------------------------- */

zu_codec zu_codec_lookup(const char *name)
{
    if (name == NULL) {
        return ZU_CODEC_NONE;
    }
    for (size_t i = 0; i < ZU_INT_N_DECLARED; i++) {
        if (strcmp(zu_int_declared[i].name, name) == 0) {
            return zu_int_declared[i].codec;
        }
    }
    /* Third-party codecs are not in the declared table; they are known only
       once registered. */
    for (size_t i = 0; i < zu_int_registry_n; i++) {
        if (strcmp(zu_int_registry[i]->name, name) == 0) {
            return (zu_codec) zu_int_registry[i]->codec;
        }
    }
    return ZU_CODEC_NONE;
}

/* ASCII-only, locale-independent. HTTP tokens are case-insensitive, and
   tolower() from <ctype.h> would consult the locale for bytes we have
   already restricted to ASCII. */
static int zu_int_ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned char ca = (unsigned char) *a;
        unsigned char cb = (unsigned char) *b;
        if (ca >= 'A' && ca <= 'Z') { ca = (unsigned char) (ca - 'A' + 'a'); }
        if (cb >= 'A' && cb <= 'Z') { cb = (unsigned char) (cb - 'A' + 'a'); }
        if (ca != cb) { return 0; }
        if (ca == '\0') { return 1; }
    }
}

zu_codec zu_codec_from_content_encoding(const char *token)
{
    if (token == NULL) {
        return ZU_CODEC_NONE;
    }
    for (size_t i = 0; i < ZU_INT_N_DECLARED; i++) {
        const char *ce = zu_int_declared[i].content_encoding;
        if (ce != NULL && zu_int_ieq(ce, token)) {
            return zu_int_declared[i].codec;
        }
    }
    for (size_t i = 0; i < zu_int_registry_n; i++) {
        const char *ce = zu_int_registry[i]->content_encoding;
        if (ce != NULL && zu_int_ieq(ce, token)) {
            return (zu_codec) zu_int_registry[i]->codec;
        }
    }
    return ZU_CODEC_NONE;
}

int zu_codec_available(zu_codec codec)
{
    return zu_int_registry_lookup(codec) != NULL;
}

zu_status zu_codec_get_info(zu_codec codec, zu_codec_info *out)
{
    if (out == NULL || out->struct_size < sizeof(zu_codec_info)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    const zu_codec_vtable *v = zu_int_registry_lookup(codec);
    if (v == NULL) {
        return ZU_ERR_UNSUPPORTED;
    }

    out->codec            = (zu_codec) v->codec;
    out->name             = v->name;
    out->content_encoding = v->content_encoding;
    out->source           = v->source;
    out->level_min        = v->level_min;
    out->level_max        = v->level_max;
    out->level_default    = v->level_default;
    out->flags            = v->flags;
    out->detectable       = (v->magic != NULL || v->sniff != NULL);
    return ZU_OK;
}

zu_status zu_codec_list(zu_codec *out, size_t cap, size_t *n_out)
{
    if (n_out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *n_out = zu_int_registry_n;
    if (out == NULL) {
        return ZU_OK;              /* count-only probe */
    }
    if (cap < zu_int_registry_n) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < zu_int_registry_n; i++) {
        out[i] = (zu_codec) zu_int_registry[i]->codec;
    }
    return ZU_OK;
}

/* Detection is a registry property, not a hardcoded gzip/zlib check: a
   satellite codec that advertises magic becomes detectable the moment it
   registers, with no change here.
 *
 * Order matters and is not arbitrary. Fixed magic is tested first, longest
 * first, so a short magic cannot shadow a longer one that also matches.
 * Predicate sniffers go last because a predicate is a weak check -- zlib's
 * accepts roughly one random byte pair in a thousand -- and should never
 * pre-empt a codec that matched a literal constant.
 *
 * A codec with neither magic nor sniffer is never detected. That is the
 * rule that keeps `auto` from ever resolving to raw DEFLATE, identity or
 * brotli: guessing wrong there means silently returning the wrong bytes. */
zu_status zu_sniff(const uint8_t *buf, size_t n, zu_codec *out)
{
    if (out == NULL || (buf == NULL && n != 0)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *out = ZU_CODEC_NONE;

    size_t best_len = 0;
    for (size_t i = 0; i < zu_int_registry_n; i++) {
        const zu_codec_vtable *v = zu_int_registry[i];
        if (v->magic == NULL || v->magic_len == 0) {
            continue;
        }
        size_t end;
        if (zu_int_add(v->magic_offset, v->magic_len, &end) != ZU_OK || n < end) {
            continue;
        }
        if (memcmp(buf + v->magic_offset, v->magic, v->magic_len) != 0) {
            continue;
        }
        if (v->magic_len > best_len) {
            best_len = v->magic_len;
            *out = (zu_codec) v->codec;
        }
    }
    if (*out != ZU_CODEC_NONE) {
        return ZU_OK;
    }

    for (size_t i = 0; i < zu_int_registry_n; i++) {
        const zu_codec_vtable *v = zu_int_registry[i];
        if (v->sniff == NULL) {
            continue;
        }
        if (v->sniff(buf, n)) {
            *out = (zu_codec) v->codec;
            return ZU_OK;
        }
    }
    return ZU_ERR_UNSUPPORTED;   /* nothing recognised these bytes */
}

zu_status zu_int_register_builtin_codecs(void)
{
    static const zu_codec_vtable *const builtin[] = {
        &zu_int_codec_identity,
        &zu_int_codec_deflate_raw,
        &zu_int_codec_zlib,
        &zu_int_codec_gzip
    };
    for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) {
        zu_status st = zu_register_codec(builtin[i]);
        if (st != ZU_OK) {
            return st;
        }
    }
    return ZU_OK;
}
