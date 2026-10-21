/* LZ4, as two codecs: `lz4-frame` (the LZ4 frame format, magic
 * 04 22 4D 18) and `lz4-block` (one bare LZ4 block, no header at all).
 *
 * They are distinct identities for the reason gzip and deflate-raw are
 * (design 3): different bytes on the wire, and only one of them is
 * detectable.
 *
 * Levels follow liblz4's frame API, for both codecs: 1 (the default) is the
 * fast compressor, 2..12 the HC compressor (LZ4HC_CLEVEL_MIN is 2), and a
 * negative level -N is the fast compressor at acceleration N + 1 -- the
 * inverted axis design 4 warns about, which is why "fast" and "best" are
 * declared below rather than read off the range. Level 0 behaves as 1, as
 * in liblz4.
 *
 * lz4-frame streams in both directions through LZ4F_*. The decoder is fed
 * whatever windows the driver offers, as liblz4 allows. The encoder cannot
 * be: LZ4F_compressUpdate() requires room for its worst case in one go, so
 * it writes into a staging buffer that is then drained into the caller's
 * window, a byte at a time if need be.
 *
 * lz4-block cannot stream at all: a block has no length and no end marker,
 * so neither direction can start before it has the whole input. Both buffer
 * the input until ZU_FINISH and then work in one pass, directly in the
 * caller's window when it is large enough -- the one-shot case -- and
 * through a staging buffer otherwise. */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lz4.h"
#include "lz4hc.h"
#define LZ4F_STATIC_LINKING_ONLY   /* LZ4F_getErrorCode() */
#include "lz4frame.h"
#include "zu_internal.h"

/* Input handed to LZ4F_compressUpdate() per call, and so what the staging
   buffer is sized for. One default (64 KiB) block. */
#define ZU_INT_LZ4F_CHUNK ((size_t) 64 * 1024)

/* The level range both codecs accept. The bottom is the level at which
   liblz4's acceleration (-level + 1) reaches LZ4_ACCELERATION_MAX, 65537;
   anything lower would behave identically. The top is LZ4HC_CLEVEL_MAX. */
#define ZU_INT_LZ4_LEVEL_MIN     (-65536)
#define ZU_INT_LZ4_LEVEL_MAX     LZ4HC_CLEVEL_MAX
#define ZU_INT_LZ4_LEVEL_DEFAULT 1

static const uint8_t zu_int_lz4_no_input[1] = { 0 };
static uint8_t       zu_int_lz4_no_output[1];

typedef struct {
    int           encoder;
    int32_t       level;

    /* lz4-frame */
    LZ4F_cctx    *cctx;
    LZ4F_dctx    *dctx;
    LZ4F_preferences_t prefs;
    int           started;     /* encoder: frame header produced */
    int           ended;       /* encoder: frame end mark produced */
    zu_int_frames frames;      /* decoder: frame continuation */

    /* Staging: bytes produced but not yet delivered. lz4-frame's encoder
       and both lz4-block directions use it. */
    uint8_t      *stage;
    size_t        stage_cap;
    size_t        stage_len;
    size_t        stage_pos;

    /* lz4-block: input gathered until ZU_FINISH */
    uint8_t      *in;
    size_t        in_cap;
    size_t        in_len;
    int           done;
    uint64_t      max_output;  /* decoder */
    int           has_expected;
    uint64_t      expected;
} lz4_state;

static void lz4_free(void *st)
{
    lz4_state *s = st;
    if (s == NULL) {
        return;
    }
    if (s->cctx != NULL) { LZ4F_freeCompressionContext(s->cctx); }
    if (s->dctx != NULL) { LZ4F_freeDecompressionContext(s->dctx); }
    free(s->stage);
    free(s->in);
    free(s);
}

static int zu_int_lz4_level(int32_t level)
{
    return (level == ZU_LEVEL_DEFAULT) ? ZU_INT_LZ4_LEVEL_DEFAULT : (int) level;
}

/* Moves staged bytes into the caller's window. Returns non-zero once the
   stage is empty. */
static int zu_int_lz4_drain(lz4_state *s, zu_buffer *buf)
{
    size_t pending = s->stage_len - s->stage_pos;
    size_t room    = buf->dst_size - buf->dst_pos;
    size_t n       = (pending < room) ? pending : room;
    if (n > 0) {
        memcpy(buf->dst + buf->dst_pos, s->stage + s->stage_pos, n);
        buf->dst_pos  += n;
        s->stage_pos  += n;
    }
    if (s->stage_pos == s->stage_len) {
        s->stage_pos = s->stage_len = 0;
        return 1;
    }
    return 0;
}

/* -- lz4-frame: status mapping ------------------------------------------- */

static zu_status zu_int_lz4f_error(const lz4_state *s, size_t r)
{
    switch (LZ4F_getErrorCode(r)) {
    case LZ4F_ERROR_allocation_failed:
        return ZU_ERR_MEMORY;
    case LZ4F_ERROR_blockChecksum_invalid:
    case LZ4F_ERROR_headerChecksum_invalid:
    case LZ4F_ERROR_contentChecksum_invalid:
        return ZU_ERR_CHECKSUM;
    case LZ4F_ERROR_frameHeader_incomplete:
        return ZU_ERR_TRUNCATED;
    case LZ4F_ERROR_srcSize_tooLarge:
    case LZ4F_ERROR_parameter_invalid:
    case LZ4F_ERROR_compressionLevel_invalid:
        return s->encoder ? ZU_ERR_INVALID_ARGUMENT : ZU_ERR_INVALID_DATA;
    default:
        /* frameType_unknown, headerVersion_wrong, reservedFlag_set,
           maxBlockSize_invalid, decompressionFailed, frameSize_wrong (a
           content size that disagrees with the decoded bytes), ... */
        return s->encoder ? ZU_ERR_INTERNAL : ZU_ERR_INVALID_DATA;
    }
}

/* -- lz4-frame: encoder -------------------------------------------------- */

static void zu_int_lz4f_prefs(lz4_state *s)
{
    memset(&s->prefs, 0, sizeof(s->prefs));
    s->prefs.compressionLevel = zu_int_lz4_level(s->level);
    /* A content checksum (XXH32 of the decoded bytes) in every frame, as
       the lz4 command-line tool writes by default: the end-to-end check
       design 17 requires be verified wherever the format has one. */
    s->prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
}

static zu_status lz4f_encoder_new(void **out, const zu_encoder_opts *o)
{
    if (out == NULL || o == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    lz4_state *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return ZU_ERR_MEMORY;
    }
    s->encoder = 1;
    s->level   = o->level;
    zu_int_lz4f_prefs(s);
    if (LZ4F_isError(LZ4F_createCompressionContext(&s->cctx, LZ4F_VERSION))) {
        lz4_free(s);
        return ZU_ERR_MEMORY;
    }
    /* Covers LZ4F_compressUpdate() of one chunk with a block already
       buffered, LZ4F_flush() and LZ4F_compressEnd() -- the bound accounts
       for all three -- and the frame header, which is at most 19 bytes. */
    size_t cap = LZ4F_compressBound(ZU_INT_LZ4F_CHUNK, &s->prefs);
    if (cap < LZ4F_HEADER_SIZE_MAX) {
        cap = LZ4F_HEADER_SIZE_MAX;
    }
    s->stage = malloc(cap);
    if (s->stage == NULL) {
        lz4_free(s);
        return ZU_ERR_MEMORY;
    }
    s->stage_cap = cap;
    *out = s;
    return ZU_OK;
}

static zu_status lz4f_encoder_reset(void *st, const zu_encoder_opts *o)
{
    lz4_state *s = st;
    if (s == NULL || s->cctx == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (o != NULL && o->level != ZU_LEVEL_DEFAULT) {
        s->level = o->level;
    }
    /* LZ4F_compressBegin() starts a fresh frame on the same context, so a
       reset is a matter of forgetting where the last one got to. The bound
       the stage was sized from does not depend on the level. */
    zu_int_lz4f_prefs(s);
    s->started = s->ended = s->done = 0;
    s->stage_len = s->stage_pos = 0;
    return ZU_OK;
}

static zu_status lz4f_encoder_process(void *st, zu_buffer *buf, zu_flush flush)
{
    lz4_state *s = st;
    if (s == NULL || buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    int flushed = 0;
    for (;;) {
        if (!zu_int_lz4_drain(s, buf)) {
            return ZU_NEED_OUTPUT;
        }
        if (s->done) {
            return ZU_STREAM_END;
        }
        if (s->ended) {
            s->done = 1;          /* end mark delivered */
            return ZU_STREAM_END;
        }

        size_t avail_in = buf->src_size - buf->src_pos;
        size_t r;
        if (!s->started) {
            if (flush == ZU_FINISH) {
                /* The whole input is in this call, so the frame can say how
                   long it is. Same rule as zstd's pledged size. */
                s->prefs.frameInfo.contentSize = (unsigned long long) avail_in;
            }
            r = LZ4F_compressBegin(s->cctx, s->stage, s->stage_cap, &s->prefs);
            s->started = 1;
        } else if (avail_in > 0) {
            size_t take = (avail_in < ZU_INT_LZ4F_CHUNK) ? avail_in
                                                         : ZU_INT_LZ4F_CHUNK;
            r = LZ4F_compressUpdate(s->cctx, s->stage, s->stage_cap,
                                    buf->src + buf->src_pos, take, NULL);
            if (!LZ4F_isError(r)) {
                buf->src_pos += take;   /* liblz4 takes all of it */
            }
        } else if (flush == ZU_FINISH) {
            r = LZ4F_compressEnd(s->cctx, s->stage, s->stage_cap, NULL);
            if (!LZ4F_isError(r)) {
                s->ended = 1;
            }
        } else if (flush == ZU_FLUSH && !flushed) {
            r = LZ4F_flush(s->cctx, s->stage, s->stage_cap, NULL);
            flushed = 1;
        } else {
            return ZU_NEED_INPUT;
        }
        if (LZ4F_isError(r)) {
            return zu_int_lz4f_error(s, r);
        }
        s->stage_len = r;
        s->stage_pos = 0;
    }
}

/* -- lz4-frame: decoder -------------------------------------------------- */

static zu_status lz4f_decoder_new(void **out, const zu_decoder_opts *o)
{
    if (out == NULL || o == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    lz4_state *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return ZU_ERR_MEMORY;
    }
    if (LZ4F_isError(LZ4F_createDecompressionContext(&s->dctx, LZ4F_VERSION))) {
        lz4_free(s);
        return ZU_ERR_MEMORY;
    }
    zu_int_frames_init(&s->frames, ZU_INT_FRAMES_LZ4, o->flags);
    *out = s;
    return ZU_OK;
}

static zu_status lz4f_decoder_reset(void *st, const zu_decoder_opts *o)
{
    lz4_state *s = st;
    if (s == NULL || s->dctx == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    LZ4F_resetDecompressionContext(s->dctx);
    zu_int_frames_init(&s->frames, ZU_INT_FRAMES_LZ4,
                       (o != NULL) ? o->flags : s->frames.flags);
    return ZU_OK;
}

static zu_status lz4f_feed_held(void *st, const uint8_t *p, size_t n)
{
    lz4_state *s = st;
    size_t dsize = 0, ssize = n;
    size_t r = LZ4F_decompress(s->dctx, zu_int_lz4_no_output, &dsize,
                               p, &ssize, NULL);
    if (LZ4F_isError(r)) {
        return zu_int_lz4f_error(s, r);
    }
    return (ssize == n && dsize == 0) ? ZU_OK : ZU_ERR_INTERNAL;
}

/* zu_int_frames_stalled()'s probe: one byte of output room, no input. */
static zu_status lz4f_probe(void *st, uint8_t *one, size_t *written,
                            int *frame_end)
{
    lz4_state *s = st;
    size_t dsize = 1, ssize = 0;
    size_t r = LZ4F_decompress(s->dctx, one, &dsize, zu_int_lz4_no_input,
                               &ssize, NULL);
    if (LZ4F_isError(r)) {
        return zu_int_lz4f_error(s, r);
    }
    *written   = dsize;
    *frame_end = (r == 0);
    return ZU_OK;
}

static zu_status lz4f_decoder_process(void *st, zu_buffer *buf, zu_flush flush)
{
    lz4_state *s = st;
    if (s == NULL || buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    for (;;) {
        if (!zu_int_frames_pay(&s->frames, buf)) {
            return ZU_NEED_OUTPUT;
        }
        if (s->frames.state != ZU_INT_FRAME_BODY) {
            zu_status fs = zu_int_frames_between(&s->frames, buf, flush,
                                                 lz4f_feed_held, s);
            if (fs != ZU_OK) {
                return fs;
            }
            continue;
        }

        const size_t avail_in  = buf->src_size - buf->src_pos;
        const size_t avail_out = buf->dst_size - buf->dst_pos;
        size_t ssize = avail_in, dsize = avail_out;
        /* Never NULL, even for an empty window: liblz4 computes
           `dst + size` and `src + size` unguarded. */
        size_t r = LZ4F_decompress(
            s->dctx,
            (buf->dst == NULL) ? zu_int_lz4_no_output : buf->dst + buf->dst_pos,
            &dsize,
            (buf->src == NULL) ? zu_int_lz4_no_input : buf->src + buf->src_pos,
            &ssize, NULL);
        if (LZ4F_isError(r)) {
            return zu_int_lz4f_error(s, r);
        }
        buf->src_pos += ssize;
        buf->dst_pos += dsize;

        if (r == 0) {
            /* Frame complete, checksums verified, everything flushed; the
               context is ready for another frame. */
            zu_int_frames_ended(&s->frames);
            continue;
        }
        if (ssize == 0 && dsize == 0) {
            zu_status ss = zu_int_frames_stalled(&s->frames, avail_in,
                                                 avail_out, flush,
                                                 lz4f_probe, s);
            if (ss != ZU_OK) {
                return ss;
            }
        }
    }
}

static zu_status lz4f_bound(int32_t level, size_t n, size_t *out)
{
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    LZ4F_preferences_t prefs;
    memset(&prefs, 0, sizeof(prefs));
    prefs.compressionLevel = zu_int_lz4_level(level);
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    /* With the content size declared, which only lengthens the header and
       so only makes this a safer upper bound for a frame without one. */
    prefs.frameInfo.contentSize = (unsigned long long) n;
    size_t b = LZ4F_compressFrameBound(n, &prefs);
    if (LZ4F_isError(b)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *out = b;
    return ZU_OK;
}

/* -- lz4-block ------------------------------------------------------------ */

static zu_status lz4b_new(void **out, int encoder, int32_t level,
                          const zu_decoder_opts *d)
{
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    lz4_state *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return ZU_ERR_MEMORY;
    }
    s->encoder = encoder;
    s->level   = level;
    if (d != NULL) {
        s->max_output   = d->max_output;
        s->has_expected = (d->flags & ZU_DEC_EXPECTED_SIZE) != 0;
        s->expected     = d->expected_size;
    }
    *out = s;
    return ZU_OK;
}

static zu_status lz4b_encoder_new(void **out, const zu_encoder_opts *o)
{
    if (o == NULL) { return ZU_ERR_INVALID_ARGUMENT; }
    return lz4b_new(out, 1, o->level, NULL);
}

static zu_status lz4b_decoder_new(void **out, const zu_decoder_opts *o)
{
    if (o == NULL) { return ZU_ERR_INVALID_ARGUMENT; }
    return lz4b_new(out, 0, ZU_LEVEL_DEFAULT, o);
}

static void zu_int_lz4b_clear(lz4_state *s)
{
    s->in_len    = 0;
    s->stage_len = s->stage_pos = 0;
    s->done      = 0;
}

static zu_status lz4b_encoder_reset(void *st, const zu_encoder_opts *o)
{
    lz4_state *s = st;
    if (s == NULL) { return ZU_ERR_INVALID_ARGUMENT; }
    if (o != NULL && o->level != ZU_LEVEL_DEFAULT) { s->level = o->level; }
    zu_int_lz4b_clear(s);
    return ZU_OK;
}

static zu_status lz4b_decoder_reset(void *st, const zu_decoder_opts *o)
{
    lz4_state *s = st;
    if (s == NULL) { return ZU_ERR_INVALID_ARGUMENT; }
    if (o != NULL) {
        s->max_output   = o->max_output;
        s->has_expected = (o->flags & ZU_DEC_EXPECTED_SIZE) != 0;
        s->expected     = o->expected_size;
    }
    zu_int_lz4b_clear(s);
    return ZU_OK;
}

/* Appends what the caller offered to the gathered input. */
static zu_status zu_int_lz4b_gather(lz4_state *s, zu_buffer *buf)
{
    size_t avail = buf->src_size - buf->src_pos;
    if (avail == 0) {
        return ZU_OK;
    }
    size_t need;
    zu_status st = zu_int_add(s->in_len, avail, &need);
    if (st != ZU_OK) { return st; }
    if (need > (size_t) LZ4_MAX_INPUT_SIZE) {
        /* Past the largest block liblz4 handles, in either direction. */
        return s->encoder ? ZU_ERR_INVALID_ARGUMENT : ZU_ERR_INVALID_DATA;
    }
    if (need > s->in_cap) {
        size_t next;
        st = zu_int_grow(s->in_cap, need - s->in_cap, &next);
        if (st != ZU_OK) { return st; }
        uint8_t *bigger = realloc(s->in, next);
        if (bigger == NULL) { return ZU_ERR_MEMORY; }
        s->in = bigger;
        s->in_cap = next;
    }
    memcpy(s->in + s->in_len, buf->src + buf->src_pos, avail);
    s->in_len    += avail;
    buf->src_pos += avail;
    return ZU_OK;
}

static int zu_int_lz4b_compress(const lz4_state *s, const uint8_t *src,
                                int n, uint8_t *dst, int cap)
{
    int lv = zu_int_lz4_level(s->level);
    if (lv >= LZ4HC_CLEVEL_MIN) {
        return LZ4_compress_HC((const char *) src, (char *) dst, n, cap, lv);
    }
    int accel = (lv < 0) ? -lv + 1 : 1;
    return LZ4_compress_fast((const char *) src, (char *) dst, n, cap, accel);
}

static zu_status zu_int_lz4b_stage(lz4_state *s, size_t n)
{
    if (n > s->stage_cap) {
        uint8_t *p = realloc(s->stage, (n == 0) ? 1 : n);
        if (p == NULL) { return ZU_ERR_MEMORY; }
        s->stage = p;
        s->stage_cap = n;
    }
    return ZU_OK;
}

static zu_status lz4b_encoder_process(void *st, zu_buffer *buf, zu_flush flush)
{
    lz4_state *s = st;
    if (s == NULL || buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (s->done) {
        return zu_int_lz4_drain(s, buf) ? ZU_STREAM_END : ZU_NEED_OUTPUT;
    }
    if (flush != ZU_FINISH) {
        /* ZU_FLUSH never gets here: the codec does not advertise it. */
        zu_status gs = zu_int_lz4b_gather(s, buf);
        return (gs != ZU_OK) ? gs : ZU_NEED_INPUT;
    }

    const size_t avail_in  = buf->src_size - buf->src_pos;
    const size_t avail_out = buf->dst_size - buf->dst_pos;

    /* One-shot: nothing gathered and the caller's window fits the worst
       case, so compress straight from their input into their output. */
    if (s->in_len == 0 && avail_in <= (size_t) LZ4_MAX_INPUT_SIZE) {
        int bound = LZ4_compressBound((int) avail_in);
        if (bound > 0 && avail_out >= (size_t) bound) {
            const uint8_t *src = (avail_in == 0 || buf->src == NULL)
                               ? zu_int_lz4_no_input : buf->src + buf->src_pos;
            int w = zu_int_lz4b_compress(s, src, (int) avail_in,
                                         buf->dst + buf->dst_pos, bound);
            if (w <= 0) {
                return ZU_ERR_INTERNAL;
            }
            buf->src_pos += avail_in;
            buf->dst_pos += (size_t) w;
            s->done = 1;
            return ZU_STREAM_END;
        }
    }

    zu_status gs = zu_int_lz4b_gather(s, buf);
    if (gs != ZU_OK) {
        return gs;
    }
    int bound = LZ4_compressBound((int) s->in_len);
    if (bound <= 0) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    gs = zu_int_lz4b_stage(s, (size_t) bound);
    if (gs != ZU_OK) {
        return gs;
    }
    const uint8_t *src = (s->in_len == 0) ? zu_int_lz4_no_input : s->in;
    int w = zu_int_lz4b_compress(s, src, (int) s->in_len, s->stage, bound);
    if (w <= 0) {
        return ZU_ERR_INTERNAL;
    }
    s->stage_len = (size_t) w;
    s->stage_pos = 0;
    s->done = 1;
    free(s->in);              /* the input is no longer needed */
    s->in = NULL;
    s->in_cap = s->in_len = 0;
    return zu_int_lz4_drain(s, buf) ? ZU_STREAM_END : ZU_NEED_OUTPUT;
}

/* The decoder's search for the size of a block nobody told it the size of.
 *
 * LZ4_decompress_safe() succeeds only when the output capacity is at least
 * the block's decoded size, and reports failure the same way whether the
 * capacity was short or the block corrupt. So: try a capacity, and when it
 * fails ask LZ4_decompress_safe_partial() -- which stops at its target and
 * validates only what it decoded -- whether the block is well-formed at
 * least that far. If not, or if it ended short of the capacity, the block
 * is corrupt and the search stops there, at a small allocation. If it
 * filled the capacity, there is more: grow 4x and try again, up to a
 * ceiling of max_output, and never more than what the format can produce
 * from n bytes (one input byte yields at most 255 output bytes). A valid
 * block therefore costs a few passes and memory up to its own size; a
 * caller who knows the size passes ZU_DEC_EXPECTED_SIZE and pays for
 * exactly one. */
static zu_status zu_int_lz4b_search(lz4_state *s, size_t *out_len)
{
    const size_t n = s->in_len;
    size_t ceiling;
    if (zu_int_mul(n, 255, &ceiling) != ZU_OK ||
        zu_int_add(ceiling, 64, &ceiling) != ZU_OK ||
        ceiling > (size_t) INT_MAX) {
        ceiling = (size_t) INT_MAX;
    }
    int limited = 0;   /* is max_output what stops the search? */
    if (s->max_output != 0 && s->max_output < (uint64_t) ceiling) {
        ceiling = (size_t) s->max_output;
        limited = 1;
    }

    size_t cap = n * 4;   /* cannot overflow: n <= LZ4_MAX_INPUT_SIZE */
    if (cap < 4096) { cap = 4096; }
    if (cap > ceiling) { cap = ceiling; }

    for (;;) {
        zu_status st = zu_int_lz4b_stage(s, cap);
        if (st != ZU_OK) { return st; }
        char *dst = (cap == 0) ? (char *) zu_int_lz4_no_output
                               : (char *) s->stage;
        int r = LZ4_decompress_safe((const char *) s->in, dst, (int) n,
                                    (int) cap);
        if (r >= 0) {
            *out_len = (size_t) r;
            return ZU_OK;
        }
        int p = LZ4_decompress_safe_partial((const char *) s->in, dst,
                                            (int) n, (int) cap, (int) cap);
        if (p < 0 || (size_t) p < cap) {
            return ZU_ERR_INVALID_DATA;   /* corrupt within the first cap */
        }
        if (cap >= ceiling) {
            /* Well-formed as far as the ceiling and still going. Under
               max_output that is the limit refusing the block. */
            return limited ? ZU_ERR_OUTPUT_LIMIT : ZU_ERR_INVALID_DATA;
        }
        cap = (cap > ceiling / 4) ? ceiling : cap * 4;
    }
}

static zu_status lz4b_decoder_process(void *st, zu_buffer *buf, zu_flush flush)
{
    lz4_state *s = st;
    if (s == NULL || buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (s->done) {
        return zu_int_lz4_drain(s, buf) ? ZU_STREAM_END : ZU_NEED_OUTPUT;
    }
    if (flush != ZU_FINISH) {
        zu_status gs = zu_int_lz4b_gather(s, buf);
        return (gs != ZU_OK) ? gs : ZU_NEED_INPUT;
    }

    if (s->has_expected) {
        /* Refuse before allocating: the size is the caller's, but it must
           still fit the limits the caller set. The driver would reach the
           same answer, only after the allocation. */
        if (s->max_output != 0 && s->expected > s->max_output) {
            return ZU_ERR_OUTPUT_LIMIT;
        }
        if (s->expected > (uint64_t) INT_MAX) {
            return ZU_ERR_INVALID_DATA;   /* no LZ4 block decodes to this */
        }
        const size_t want = (size_t) s->expected;
        const size_t avail_in  = buf->src_size - buf->src_pos;
        const size_t avail_out = buf->dst_size - buf->dst_pos;

        /* One-shot: decode straight into the caller's window. */
        if (s->in_len == 0 && avail_out >= want) {
            if (avail_in == 0 || avail_in > (size_t) LZ4_MAX_INPUT_SIZE) {
                return ZU_ERR_INVALID_DATA;
            }
            int r = LZ4_decompress_safe(
                (const char *) (buf->src + buf->src_pos),
                (char *) ((buf->dst == NULL) ? zu_int_lz4_no_output
                                             : buf->dst + buf->dst_pos),
                (int) avail_in, (int) want);
            if (r < 0 || (size_t) r != want) {
                return ZU_ERR_INVALID_DATA;
            }
            buf->src_pos += avail_in;
            buf->dst_pos += want;
            s->done = 1;
            return ZU_STREAM_END;
        }

        zu_status gs = zu_int_lz4b_gather(s, buf);
        if (gs != ZU_OK) { return gs; }
        if (s->in_len == 0) { return ZU_ERR_INVALID_DATA; }
        gs = zu_int_lz4b_stage(s, want);
        if (gs != ZU_OK) { return gs; }
        int r = LZ4_decompress_safe((const char *) s->in,
                                    (char *) ((want == 0) ? zu_int_lz4_no_output
                                                          : s->stage),
                                    (int) s->in_len, (int) want);
        if (r < 0 || (size_t) r != want) {
            return ZU_ERR_INVALID_DATA;
        }
        s->stage_len = want;
    } else {
        zu_status gs = zu_int_lz4b_gather(s, buf);
        if (gs != ZU_OK) { return gs; }
        /* An LZ4 block is at least one token byte, even for no data. */
        if (s->in_len == 0) { return ZU_ERR_INVALID_DATA; }
        size_t got = 0;
        gs = zu_int_lz4b_search(s, &got);
        if (gs != ZU_OK) { return gs; }
        s->stage_len = got;
    }

    s->stage_pos = 0;
    s->done = 1;
    free(s->in);
    s->in = NULL;
    s->in_cap = s->in_len = 0;
    return zu_int_lz4_drain(s, buf) ? ZU_STREAM_END : ZU_NEED_OUTPUT;
}

static zu_status lz4b_bound(int32_t level, size_t n, size_t *out)
{
    (void) level;
    if (out == NULL || n > (size_t) LZ4_MAX_INPUT_SIZE) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    int b = LZ4_compressBound((int) n);
    if (b <= 0) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *out = (size_t) b;
    return ZU_OK;
}

/* -- vtables ------------------------------------------------------------- */

/* The LZ4 frame format's magic, 0x184D2204, little-endian. */
static const uint8_t zu_int_lz4f_magic[4] = { 0x04, 0x22, 0x4D, 0x18 };

const zu_codec_vtable zu_int_codec_lz4_frame = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_LZ4_FRAME,
    "lz4-frame",
    NULL,                 /* not an HTTP content-coding */
    "zukomp",
    ZU_INT_LZ4_LEVEL_MIN, ZU_INT_LZ4_LEVEL_MAX, ZU_INT_LZ4_LEVEL_DEFAULT,
    ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH,
    zu_int_lz4f_magic, sizeof(zu_int_lz4f_magic), 0, NULL,
    lz4f_encoder_new, lz4f_encoder_process, lz4f_encoder_reset, lz4_free,
    lz4f_decoder_new, lz4f_decoder_process, lz4f_decoder_reset, lz4_free,
    lz4f_bound,
    /* level_fast / level_best. "fast" is the default fast compressor, not a
       negative level: those trade ratio away steeply, and "fast" should
       still mean "compress". "best" is the top of the HC range. */
    1, LZ4HC_CLEVEL_MAX
};

const zu_codec_vtable zu_int_codec_lz4_block = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_LZ4_BLOCK,
    "lz4-block",
    NULL,
    "zukomp",
    ZU_INT_LZ4_LEVEL_MIN, ZU_INT_LZ4_LEVEL_MAX, ZU_INT_LZ4_LEVEL_DEFAULT,
    /* No ZU_CAN_FLUSH: a block cannot be emitted until all of it is known. */
    ZU_CAN_ENCODE | ZU_CAN_DECODE,
    NULL, 0, 0, NULL,     /* headerless, so never detectable (design 5) */
    lz4b_encoder_new, lz4b_encoder_process, lz4b_encoder_reset, lz4_free,
    lz4b_decoder_new, lz4b_decoder_process, lz4b_decoder_reset, lz4_free,
    lz4b_bound,
    1, LZ4HC_CLEVEL_MAX
};
