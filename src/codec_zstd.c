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

#define ZSTD_STATIC_LINKING_ONLY   /* ZSTD_getFrameHeader(), ZSTD_FRAMEHEADERSIZE_MAX */
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
    int         window_log;   /* decoder: the ceiling zu_int_zstd_window()
                                 chose, so a refusal can say whether the
                                 cap or the build set it */
    zu_int_frames frames;     /* decoder: frame continuation */
    /* decoder: the current frame's header, collected before libzstd sees
       any of it, so the window ceiling can be set for this frame
       (zu_int_zstd_header()). */
    uint8_t     hdr[ZSTD_FRAMEHEADERSIZE_MAX];
    size_t      hdr_n;
    int         hdr_open;     /* at a frame start, header not yet complete */
    int         done;         /* encoder: the frame has been terminated */
} zstd_state;

/* -- status mapping ------------------------------------------------------ */

/* libzstd counts consecutive ZSTD_decompressStream() calls that move
   nothing and, from the ZSTD_NO_FORWARD_PROGRESS_MAX-th (16), reports
   noForwardProgress_inputEmpty or _destFull -- a guard against a caller
   spinning. It is not an error in the stream: the frame state is untouched
   and the next call that moves a byte resets the count. For zukomp a
   caller that waits is legal however long it waits (design 8), and
   "nothing moved" already has its own handling, so these two are that
   case, not ZU_ERR_INVALID_DATA. */
static int zu_int_zstd_no_progress(size_t r)
{
    if (!ZSTD_isError(r)) {
        return 0;
    }
    const ZSTD_ErrorCode e = ZSTD_getErrorCode(r);
    return e == ZSTD_error_noForwardProgress_inputEmpty ||
           e == ZSTD_error_noForwardProgress_destFull;
}

static zu_status zu_int_zstd_error(const zstd_state *s, size_t r)
{
    switch (ZSTD_getErrorCode(r)) {
    case ZSTD_error_checksum_wrong:
        return ZU_ERR_CHECKSUM;
    case ZSTD_error_memory_allocation:
        return ZU_ERR_MEMORY;
    case ZSTD_error_frameParameter_windowTooLarge:
        /* The frame asks for a larger window than the decoder allows, and
           the class says which ceiling refused it (design 20). When
           max_output brought the ceiling below the build's own, the limit
           refused the stream -- the window is memory the input claimed,
           and design 20 forbids allocating on the input's word alone.
           Otherwise the ceiling is libzstd's own 128 MiB, which this build
           does not lift, and no limit could admit the frame: under
           max_output = 2^30 a 2^28 window used to be reported as the
           output limit, where without a limit the same bytes were
           unsupported. libzstd does not say how large the window was, so
           under a limit of 2^26 or less a window past 2^27 is still over
           the limit: true, if not the whole story. The encoder never sees
           this error. */
        if (s->encoder) {
            return ZU_ERR_INTERNAL;
        }
        return (s->window_log < ZU_INT_ZSTD_WINDOWLOG_DEFAULT)
                   ? ZU_ERR_OUTPUT_LIMIT : ZU_ERR_UNSUPPORTED;
    case ZSTD_error_dictionary_wrong:
    case ZSTD_error_version_unsupported:
        /* Well-formed, but needs something this build does not have: a
           dictionary, a legacy format. Same reasoning as zlib's FDICT.
           frameParameter_unsupported is not here: libzstd raises it for a
           reserved header bit that is set, a skippable frame whose size
           overflows, and a non-skippable frame where a skippable one was
           expected -- all malformed input, so invalid data, as lz4's
           reservedFlag_set is (design 7). */
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
static int zu_int_zstd_ceiling(const zstd_state *s)
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
    return wlog;
}

static zu_status zu_int_zstd_set_window(zstd_state *s, int wlog)
{
    size_t r = ZSTD_DCtx_setParameter(s->dctx, ZSTD_d_windowLogMax, wlog);
    if (ZSTD_isError(r)) {
        return ZU_ERR_INTERNAL;
    }
    s->window_log = wlog;
    return ZU_OK;
}

static zu_status zu_int_zstd_window(zstd_state *s)
{
    s->hdr_n    = 0;
    s->hdr_open = 1;
    return zu_int_zstd_set_window(s, zu_int_zstd_ceiling(s));
}

/* The ceiling is about memory, and the memory a frame costs is not always
   its window. libzstd's streaming decoder sizes its buffer to the smaller
   of the window and the frame's declared content size
   (ZSTD_decodingBufferSize_internal()), yet refuses on the window alone --
   while its single-pass path, taken only when the whole frame is in the
   input and the output room covers the content size, never looks at the
   window. A valid frame declaring a large window and a small content size
   therefore decoded or was refused depending on how much output room the
   first call happened to offer (#119).
 *
 * zukomp applies one rule instead: a frame is refused when the smaller of
 * its window and its declared content size is above the ceiling. A frame
 * that declares a content size within the ceiling gets libzstd's largest
 * window for this frame, since its buffer cannot exceed that content size.
 * Every frame now goes through the streaming path, because libzstd sees
 * the header on its own, before the rest of the frame. */
static zu_status zu_int_zstd_frame_window(zstd_state *s,
                                          const ZSTD_FrameHeader *h)
{
    int wlog = zu_int_zstd_ceiling(s);
    if (h->frameType == ZSTD_frame &&
        h->frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN &&
        h->frameContentSize <= ((uint64_t) 1 << wlog)) {
        wlog = ZSTD_WINDOWLOG_MAX;
    }
    return zu_int_zstd_set_window(s, wlog);
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

/* Hands libzstd bytes it has not seen yet. They are all or part of a frame
   header, so it buffers them and writes nothing. */
static zu_status zu_int_zstd_feed(zstd_state *s, const uint8_t *p, size_t n)
{
    ZSTD_inBuffer  in  = { p, n, 0 };
    ZSTD_outBuffer out = { zu_int_zstd_no_output, 0, 0 };
    size_t r = ZSTD_decompressStream(s->dctx, &out, &in);
    if (ZSTD_isError(r)) {
        return zu_int_zstd_error(s, r);
    }
    return (in.pos == n) ? ZU_OK : ZU_ERR_INTERNAL;
}

/* The frame prober's held magic: the start of the next frame's header. */
static zu_status zstd_feed_held(void *st, const uint8_t *p, size_t n)
{
    zstd_state *s = st;
    if (!s->hdr_open || s->hdr_n + n > sizeof(s->hdr)) {
        return ZU_ERR_INTERNAL;
    }
    memcpy(s->hdr + s->hdr_n, p, n);
    s->hdr_n += n;
    return ZU_OK;
}

/* Collects the frame header from the input, one byte at a time, and hands
   it to libzstd once complete with this frame's window set. ZU_OK means
   libzstd has the header (or the bytes libzstd will refuse) and decoding
   can go on; anything else is process()'s status.
 *
 * A header ZSTD_getFrameHeader() calls malformed goes to libzstd as it is,
 * so the classification stays libzstd's. Input that ends inside a header
 * is the same truncation it was when libzstd held the bytes itself. */
static zu_status zu_int_zstd_header(zstd_state *s, zu_buffer *buf,
                                    zu_flush flush)
{
    for (;;) {
        ZSTD_FrameHeader h;
        size_t need = ZSTD_getFrameHeader(&h, s->hdr, s->hdr_n);
        if (ZSTD_isError(need) || need == 0) {
            zu_status st = ZSTD_isError(need) ? ZU_OK : zu_int_zstd_frame_window(s, &h);
            s->hdr_open = 0;
            if (st == ZU_OK && s->hdr_n > 0) {
                st = zu_int_zstd_feed(s, s->hdr, s->hdr_n);
            }
            s->hdr_n = 0;
            return st;
        }
        if (buf->src_pos == buf->src_size) {
            return (flush == ZU_FINISH) ? ZU_ERR_TRUNCATED : ZU_NEED_INPUT;
        }
        s->hdr[s->hdr_n++] = buf->src[buf->src_pos++];
    }
}

/* zu_int_frames_stalled()'s probe: one byte of output room, no input. */
static zu_status zstd_probe(void *st, uint8_t *one, size_t *written,
                            int *frame_end)
{
    zstd_state *s = st;
    ZSTD_inBuffer  in  = { zu_int_zstd_no_input, 0, 0 };
    ZSTD_outBuffer out = { one, 1, 0 };
    size_t r = ZSTD_decompressStream(s->dctx, &out, &in);
    if (zu_int_zstd_no_progress(r)) {
        *written   = 0;          /* nothing pending, frame not ended */
        *frame_end = 0;
        return ZU_OK;
    }
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
            /* Between frames: done, or decide whether another follows. Any
               frame end lands here, whichever site saw it (the r == 0 below
               or zu_int_frames_stalled()'s probe), so this is the one place
               the next frame's header collection is armed. */
            s->hdr_n    = 0;
            s->hdr_open = 1;
            zu_status fs = zu_int_frames_between(&s->frames, buf, flush,
                                                 zstd_feed_held, s);
            if (fs != ZU_OK) {
                return fs;   /* ZU_STREAM_END, ZU_NEED_INPUT or an error */
            }
            continue;        /* another frame begins */
        }
        if (s->hdr_open) {
            zu_status hs = zu_int_zstd_header(s, buf, flush);
            if (hs != ZU_OK) {
                return hs;
            }
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
        if (zu_int_zstd_no_progress(r)) {
            r = 1;               /* not the frame's end: the stall below */
        } else if (ZSTD_isError(r)) {
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
