/* The identity codec: bytes in, the same bytes out.
 *
 * It earns its place three ways. It makes a pipeline that may or may not
 * compress uniform, so callers need no special case. It is the HTTP
 * `identity` content-coding. And it lets Stage 4 test the stream driver,
 * the growing output buffer and every security limit before a real codec
 * exists to be blamed for a failure.
 *
 * It holds no state: both directions are the same copy loop, so one
 * implementation serves as encoder and decoder. */
#include <string.h>

#include "zu_internal.h"

/* A non-NULL, never-dereferenced handle. The core requires *st to be
   non-NULL so it can distinguish "created" from "failed", but identity has
   nothing to remember between calls. */
static int zu_int_identity_state;

static zu_status identity_new(void **st)
{
    if (st == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *st = &zu_int_identity_state;
    return ZU_OK;
}

static zu_status identity_encoder_new(void **st, const zu_encoder_opts *opts)
{
    (void) opts;
    return identity_new(st);
}

static zu_status identity_decoder_new(void **st, const zu_decoder_opts *opts)
{
    (void) opts;
    return identity_new(st);
}

/* Copies as much as both cursors allow, then reports what stopped it.
 *
 * The status is about progress, not about the copy: ZU_STREAM_END only when
 * the caller said no more input follows and none is left, ZU_NEED_OUTPUT
 * when input remains but the output buffer is full, ZU_NEED_INPUT
 * otherwise. Getting this wrong would spin the driver's loop, so the
 * chunk-boundary sweeps in Stage 4 exist to pin it down. */
static zu_status identity_process(void *st, zu_buffer *buf, zu_flush flush)
{
    (void) st;

    if (buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (buf->src_pos > buf->src_size || buf->dst_pos > buf->dst_size) {
        return ZU_ERR_INVALID_ARGUMENT;
    }

    size_t avail_in  = buf->src_size - buf->src_pos;
    size_t avail_out = buf->dst_size - buf->dst_pos;
    size_t n = (avail_in < avail_out) ? avail_in : avail_out;

    if (n > 0) {
        /* src may legitimately be NULL only when avail_in is 0, which n > 0
           already excludes; memcpy(NULL, ...) is undefined even for n == 0. */
        if (buf->src == NULL || buf->dst == NULL) {
            return ZU_ERR_INVALID_ARGUMENT;
        }
        memcpy(buf->dst + buf->dst_pos, buf->src + buf->src_pos, n);
        buf->src_pos += n;
        buf->dst_pos += n;
        avail_in  -= n;
        avail_out -= n;
    }

    if (avail_in > 0) {
        return ZU_NEED_OUTPUT;
    }
    if (flush == ZU_FINISH) {
        return ZU_STREAM_END;
    }
    return ZU_NEED_INPUT;
}

static zu_status identity_encoder_reset(void *st, const zu_encoder_opts *opts)
{
    (void) st; (void) opts;
    return ZU_OK;   /* stateless: nothing to reset */
}

static zu_status identity_decoder_reset(void *st, const zu_decoder_opts *opts)
{
    (void) st; (void) opts;
    return ZU_OK;
}

static void identity_free(void *st)
{
    (void) st;      /* the handle points at static storage */
}

static zu_status identity_bound(int32_t level, size_t n, size_t *out)
{
    (void) level;
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *out = n;       /* exact: identity never grows its input */
    return ZU_OK;
}

const zu_codec_vtable zu_int_codec_identity = {
    /* struct_size      */ (uint32_t) sizeof(zu_codec_vtable),
    /* codec            */ (uint32_t) ZU_CODEC_IDENTITY,
    /* name             */ "identity",
    /* content_encoding */ "identity",
    /* source           */ "zukomp",

    /* level_min        */ 0,
    /* level_max        */ 0,
    /* level_default    */ 0,      /* all three 0: identity has no levels */
    /* flags            */ ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH,

    /* magic            */ NULL,   /* deliberately undetectable: identity is
                                      indistinguishable from arbitrary bytes,
                                      so `auto` must never resolve to it */
    /* magic_len        */ 0,
    /* magic_offset     */ 0,
    /* sniff            */ NULL,

    /* encoder_new      */ identity_encoder_new,
    /* encoder_process  */ identity_process,
    /* encoder_reset    */ identity_encoder_reset,
    /* encoder_free     */ identity_free,

    /* decoder_new      */ identity_decoder_new,
    /* decoder_process  */ identity_process,
    /* decoder_reset    */ identity_decoder_reset,
    /* decoder_free     */ identity_free,

    /* bound            */ identity_bound
};
