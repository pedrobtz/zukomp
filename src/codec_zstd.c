/* zstd (RFC 8878), over the vendored libzstd's streaming API.
 *
 * libzstd already accepts arbitrarily small input and output windows in
 * both directions -- including a zero-byte output window -- so, unlike the
 * DEFLATE family, there is no wrapper to stream here and no staging buffer:
 * each process() call turns libzstd's crank until it cannot move, then says
 * why. What zukomp adds is the classification (design 7), the frame
 * continuation policy shared with lz4-frame (src/zu_frames.c), and a decoder
 * window bounded by max_output.
 *
 * Determinism: output depends on the level, on libzstd's version, and on
 * whether the whole input arrived with ZU_FINISH on the first call. In that
 * case libzstd records the content size in the frame header and sizes its
 * tables to the input, which is both smaller output and far less memory at
 * high levels; otherwise it cannot know the size. The decoded bytes are the
 * same either way. komp_compress() always passes the whole input at once. */
#include <stdlib.h>
#include <string.h>

#include "zstd.h"
#include "zstd_errors.h"
#include "zu_internal.h"

/* Default decoder window ceiling: libzstd's own default, 2^27 = 128 MiB,
   which is also what the zstd command-line tool refuses to exceed without
   --memory. */
#define ZU_INT_ZSTD_WINDOWLOG_DEFAULT 27
/* Floor for the max_output-derived ceiling. 2^23 = 8 MiB is the largest
   window any non-"ultra" level (1..19) selects when the input size is not
   known in advance, so a valid stream made at those levels is never refused
   for its window alone, however small max_output is. */
#define ZU_INT_ZSTD_WINDOWLOG_FLOOR 23

typedef struct {
    int         encoder;
    ZSTD_CCtx  *cctx;
    ZSTD_DCtx  *dctx;
    int32_t     level;        /* encoder: the level the stream was built at */
    uint64_t    max_output;   /* decoder: for the window ceiling */
    zu_int_frames frames;     /* decoder: frame continuation */
    int         done;         /* encoder: the frame has been terminated */
} zstd_state;

/* -- status mapping ------------------------------------------------------ */

static zu_status zu_int_zstd_error(const zstd_state *s, size_t r)
{
    switch (ZSTD_getErrorCode(r)) {
    case ZSTD_error_checksum_wrong:
        return ZU_ERR_CHECKSUM;
    case ZSTD_error_memory_allocation:
        return ZU_ERR_MEMORY;
    case ZSTD_error_frameParameter_windowTooLarge:
        /* The frame asks for a larger window than the decoder allows. Under
           a max_output that is the limit refusing the stream -- the window
           is memory the input claimed, and design 20 forbids allocating on
           the input's word alone. With no limit it is libzstd's own 128 MiB
           ceiling, which this build does not lift. */
        return (s->max_output != 0) ? ZU_ERR_OUTPUT_LIMIT : ZU_ERR_UNSUPPORTED;
    case ZSTD_error_dictionary_wrong:
    case ZSTD_error_frameParameter_unsupported:
    case ZSTD_error_version_unsupported:
        /* Well-formed, but needs something this build does not have: a
           dictionary, a reserved feature, a legacy format. Same reasoning
           as zlib's FDICT. */
        return ZU_ERR_UNSUPPORTED;
    case ZSTD_error_parameter_unsupported:
    case ZSTD_error_parameter_outOfBound:
        return ZU_ERR_INVALID_ARGUMENT;
    default:
        /* prefix_unknown, corruption_detected, srcSize_wrong, ... */
        return s->encoder ? ZU_ERR_INTERNAL : ZU_ERR_INVALID_DATA;
    }
}

/* ceil(log2(n)) for n >= 1. */
static int zu_int_ceil_log2(uint64_t n)
{
    int b = 0;
    while (b < 63 && ((uint64_t) 1 << b) < n) {
        b++;
    }
    return b;
}

/* The decoder window is memory the frame header claims, before a single
   byte of output exists. libzstd caps it at 128 MiB by default; under a
   max_output smaller than that, the cap comes down to the next power of two
   at or above max_output, never below 8 MiB. */
static zu_status zu_int_zstd_window(zstd_state *s)
{
    int wlog = ZU_INT_ZSTD_WINDOWLOG_DEFAULT;
    if (s->max_output != 0) {
        int want = zu_int_ceil_log2(s->max_output);
        if (want < ZU_INT_ZSTD_WINDOWLOG_FLOOR) {
            want = ZU_INT_ZSTD_WINDOWLOG_FLOOR;
        }
        if (want < wlog) {
            wlog = want;
        }
    }
    size_t r = ZSTD_DCtx_setParameter(s->dctx, ZSTD_d_windowLogMax, wlog);
    return ZSTD_isError(r) ? ZU_ERR_INTERNAL : ZU_OK;
}

static zu_status zu_int_zstd_level(zstd_state *s, int32_t level)
{
    /* Level 0 is libzstd's own spelling of "default", so ZU_LEVEL_DEFAULT
       can map straight onto it -- but spelling it out keeps komp_codecs()'s
       level_default and what actually runs visibly the same number. */
    int lv = (level == ZU_LEVEL_DEFAULT) ? ZSTD_CLEVEL_DEFAULT : (int) level;
    size_t r = ZSTD_CCtx_setParameter(s->cctx, ZSTD_c_compressionLevel, lv);
    if (ZSTD_isError(r)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    s->level = (int32_t) lv;
    return ZU_OK;
}

/* -- lifecycle ----------------------------------------------------------- */

static zu_status zstd_encoder_new(void **out, const zu_encoder_opts *o)
{
    if (out == NULL || o == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    zstd_state *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return ZU_ERR_MEMORY;
    }
    s->encoder = 1;
    s->cctx = ZSTD_createCCtx();
    if (s->cctx == NULL) {
        free(s);
        return ZU_ERR_MEMORY;
    }
    zu_status st = zu_int_zstd_level(s, o->level);
    /* A content checksum (XXH64 of the decoded bytes) in every frame, as
       the zstd command-line tool writes by default. Four bytes buys every
       decoder an end-to-end integrity check, which is the property gzip's
       CRC-32 gives and design 17 requires be verified. */
    if (st == ZU_OK &&
        ZSTD_isError(ZSTD_CCtx_setParameter(s->cctx, ZSTD_c_checksumFlag, 1))) {
        st = ZU_ERR_INTERNAL;
    }
    if (st != ZU_OK) {
        ZSTD_freeCCtx(s->cctx);
        free(s);
        return st;
    }
    *out = s;
    return ZU_OK;
}

static zu_status zstd_decoder_new(void **out, const zu_decoder_opts *o)
{
    if (out == NULL || o == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    zstd_state *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return ZU_ERR_MEMORY;
    }
    s->dctx = ZSTD_createDCtx();
    if (s->dctx == NULL) {
        free(s);
        return ZU_ERR_MEMORY;
    }
    s->max_output = o->max_output;
    zu_int_frames_init(&s->frames, ZU_INT_FRAMES_ZSTD, o->flags);
    zu_status st = zu_int_zstd_window(s);
    if (st != ZU_OK) {
        ZSTD_freeDCtx(s->dctx);
        free(s);
        return st;
    }
    *out = s;
    return ZU_OK;
}

static void zstd_free(void *st)
{
    zstd_state *s = st;
    if (s == NULL) {
        return;
    }
    ZSTD_freeCCtx(s->cctx);   /* both accept NULL */
    ZSTD_freeDCtx(s->dctx);
    free(s);
}

static zu_status zstd_encoder_reset(void *st, const zu_encoder_opts *o)
{
    zstd_state *s = st;
    if (s == NULL || s->cctx == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    /* Session only: the level and the checksum flag survive, which is what
       "saying nothing must change nothing" needs for ZU_LEVEL_DEFAULT. */
    if (ZSTD_isError(ZSTD_CCtx_reset(s->cctx, ZSTD_reset_session_only))) {
        return ZU_ERR_INTERNAL;
    }
    if (o != NULL && o->level != ZU_LEVEL_DEFAULT && o->level != s->level) {
        zu_status rs = zu_int_zstd_level(s, o->level);
        if (rs != ZU_OK) {
            return rs;
        }
    }
    s->done = 0;
    return ZU_OK;
}

static zu_status zstd_decoder_reset(void *st, const zu_decoder_opts *o)
{
    zstd_state *s = st;
    if (s == NULL || s->dctx == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (ZSTD_isError(ZSTD_DCtx_reset(s->dctx, ZSTD_reset_session_only))) {
        return ZU_ERR_INTERNAL;
    }
    uint32_t flags = s->frames.flags;
    if (o != NULL) {
        s->max_output = o->max_output;
        flags = o->flags;
    }
    zu_int_frames_init(&s->frames, ZU_INT_FRAMES_ZSTD, flags);
    return zu_int_zstd_window(s);
}

/* -- process ------------------------------------------------------------- */

/* Never hand libzstd a NULL pointer, even for an empty window: it computes
   `src + pos` unguarded, which UBSan reports as arithmetic on a null
   pointer. Same remedy as codec_deflate.c's. */
static const uint8_t zu_int_zstd_no_input[1] = { 0 };
static uint8_t       zu_int_zstd_no_output[1];

static zu_status zstd_encoder_process(void *st, zu_buffer *buf, zu_flush flush)
{
    zstd_state *s = st;
    if (s == NULL || buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (s->done) {
        return ZU_STREAM_END;
    }
    const ZSTD_EndDirective op = (flush == ZU_FINISH) ? ZSTD_e_end
                               : (flush == ZU_FLUSH)  ? ZSTD_e_flush
                                                      : ZSTD_e_continue;
    for (;;) {
        const size_t avail_in  = buf->src_size - buf->src_pos;
        const size_t avail_out = buf->dst_size - buf->dst_pos;

        ZSTD_inBuffer in = {
            (buf->src == NULL) ? zu_int_zstd_no_input : buf->src + buf->src_pos,
            avail_in, 0
        };
        ZSTD_outBuffer out = {
            (buf->dst == NULL) ? zu_int_zstd_no_output : buf->dst + buf->dst_pos,
            avail_out, 0
        };
        size_t r = ZSTD_compressStream2(s->cctx, &out, &in, op);
        if (ZSTD_isError(r)) {
            return zu_int_zstd_error(s, r);
        }
        buf->src_pos += in.pos;
        buf->dst_pos += out.pos;

        const int spent = (buf->src_pos == buf->src_size);
        if (op == ZSTD_e_end && r == 0) {
            s->done = 1;
            return ZU_STREAM_END;
        }
        /* r is what remains to be flushed. For e_flush, 0 with all input
           taken means everything so far is on the wire; e_continue makes no
           such promise and is done as soon as it has taken the input. */
        if (spent && (op == ZSTD_e_continue || r == 0)) {
            return ZU_NEED_INPUT;
        }
        if (in.pos == 0 && out.pos == 0) {
            /* Could not move: libzstd always takes input or emits output
               when it has room for either, so this is the output window. */
            return (avail_out == 0) ? ZU_NEED_OUTPUT : ZU_ERR_INTERNAL;
        }
    }
}

/* Hands bytes the frame prober already consumed to libzstd. They are a
   frame magic, so libzstd buffers them as the start of a frame header and
   writes nothing. */
static zu_status zstd_feed_held(void *st, const uint8_t *p, size_t n)
{
    zstd_state *s = st;
    ZSTD_inBuffer  in  = { p, n, 0 };
    ZSTD_outBuffer out = { zu_int_zstd_no_output, 0, 0 };
    size_t r = ZSTD_decompressStream(s->dctx, &out, &in);
    if (ZSTD_isError(r)) {
        return zu_int_zstd_error(s, r);
    }
    return (in.pos == n) ? ZU_OK : ZU_ERR_INTERNAL;
}

/* zu_int_frames_stalled()'s probe: one byte of output room, no input. */
static zu_status zstd_probe(void *st, uint8_t *one, size_t *written,
                            int *frame_end)
{
    zstd_state *s = st;
    ZSTD_inBuffer  in  = { zu_int_zstd_no_input, 0, 0 };
    ZSTD_outBuffer out = { one, 1, 0 };
    size_t r = ZSTD_decompressStream(s->dctx, &out, &in);
    if (ZSTD_isError(r)) {
        return zu_int_zstd_error(s, r);
    }
    *written   = out.pos;
    *frame_end = (r == 0);
    return ZU_OK;
}

static zu_status zstd_decoder_process(void *st, zu_buffer *buf, zu_flush flush)
{
    zstd_state *s = st;
    if (s == NULL || buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    for (;;) {
        if (!zu_int_frames_pay(&s->frames, buf)) {
            return ZU_NEED_OUTPUT;
        }
        if (s->frames.state != ZU_INT_FRAME_BODY) {
            /* Between frames: done, or decide whether another follows. */
            zu_status fs = zu_int_frames_between(&s->frames, buf, flush,
                                                 zstd_feed_held, s);
            if (fs != ZU_OK) {
                return fs;   /* ZU_STREAM_END, ZU_NEED_INPUT or an error */
            }
            continue;        /* another frame begins */
        }

        const size_t avail_in  = buf->src_size - buf->src_pos;
        const size_t avail_out = buf->dst_size - buf->dst_pos;

        ZSTD_inBuffer in = {
            (buf->src == NULL) ? zu_int_zstd_no_input : buf->src + buf->src_pos,
            avail_in, 0
        };
        ZSTD_outBuffer out = {
            (buf->dst == NULL) ? zu_int_zstd_no_output : buf->dst + buf->dst_pos,
            avail_out, 0
        };
        size_t r = ZSTD_decompressStream(s->dctx, &out, &in);
        if (ZSTD_isError(r)) {
            return zu_int_zstd_error(s, r);
        }
        buf->src_pos += in.pos;
        buf->dst_pos += out.pos;

        if (r == 0) {
            /* A frame is decoded, checksum verified and fully flushed.
               libzstd starts the next frame on its own; whether zukomp lets
               it is the continuation policy's call. */
            zu_int_frames_ended(&s->frames);
            continue;
        }
        if (in.pos == 0 && out.pos == 0) {
            zu_status ss = zu_int_frames_stalled(&s->frames, avail_in,
                                                 avail_out, flush,
                                                 zstd_probe, s);
            if (ss != ZU_OK) {
                return ss;
            }
        }
    }
}

/* -- bound --------------------------------------------------------------- */

static zu_status zstd_bound(int32_t level, size_t n, size_t *out)
{
    (void) level;
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    size_t b = ZSTD_compressBound(n);
    if (ZSTD_isError(b) || b == 0) {
        return ZU_ERR_INVALID_ARGUMENT;   /* n beyond what zstd can encode */
    }
    *out = b;
    return ZU_OK;
}

/* -- vtable -------------------------------------------------------------- */

/* RFC 8878 section 3.1.1: 0xFD2FB528, little-endian. */
static const uint8_t zu_int_zstd_magic[4] = { 0x28, 0xB5, 0x2F, 0xFD };

const zu_codec_vtable zu_int_codec_zstd = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_ZSTD,
    "zstd",
    "zstd",               /* RFC 8878 section 7.2: the `zstd` content-coding */
    "zukomp",
    /* level_min is ZSTD_minCLevel(), -ZSTD_TARGETLENGTH_MAX: the negative
       levels are libzstd's "fast" accelerations. test-zstd.R checks the
       literal against the library. 0 is libzstd's own alias for its
       default, and is accepted as such. */
    -131072, 22, ZSTD_CLEVEL_DEFAULT,
    ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH,
    zu_int_zstd_magic, sizeof(zu_int_zstd_magic), 0, NULL,
    zstd_encoder_new, zstd_encoder_process, zstd_encoder_reset, zstd_free,
    zstd_decoder_new, zstd_decoder_process, zstd_decoder_reset, zstd_free,
    zstd_bound,
    /* level_fast / level_best. 1 rather than a negative level, for the
       reason DEFLATE's is 1 rather than 0: the negative levels trade ratio
       away steeply, and "fast" should still mean "compress". 19 rather than
       22 because 20..22 are what the zstd tool gates behind --ultra: with
       no size known in advance they select windows up to 128 MiB, so
       "best" would cost every decoder that much memory. */
    1, 19
};
