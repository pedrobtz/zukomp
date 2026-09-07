/* The core stream driver.
 *
 * Everything security-relevant lives here rather than in a codec, because
 * this is the one place that sees every byte of every stream through the
 * zu_buffer cursors. A codec cannot forget to enforce max_output, cannot
 * enforce it inconsistently, and a third-party codec nobody here reviewed
 * inherits the protection automatically. That is the single reason the
 * registry sits above the codecs rather than beside them (design 10).
 *
 * The driver owns stream state only. Input and output buffers belong to the
 * caller, and nothing here allocates output. */
#include <stdlib.h>
#include <string.h>

#include "zu_internal.h"

/* Both handles have the same shape; they are distinct types so that a
   decoder cannot be passed to an encoder entry point. */
struct zu_encoder {
    const zu_codec_vtable *vtable;
    void                  *state;
    zu_encoder_opts        opts;
    int                    finished;
};

struct zu_decoder {
    const zu_codec_vtable *vtable;
    void                  *state;
    zu_decoder_opts        opts;
    int                    finished;
    uint64_t               total_in;   /* cumulative, across process() calls */
    uint64_t               total_out;
};

/* -- shared validation --------------------------------------------------- */

static zu_status zu_int_check_buffer(const zu_buffer *buf)
{
    if (buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    /* A cursor past its own size means the caller corrupted the struct, or
       reused a buffer without resetting. Either way, proceeding would index
       out of bounds. */
    if (buf->src_pos > buf->src_size || buf->dst_pos > buf->dst_size) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (buf->src == NULL && buf->src_size != 0) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (buf->dst == NULL && buf->dst_size != 0) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    return ZU_OK;
}

static zu_status zu_int_check_flush(zu_flush flush)
{
    if (flush != ZU_RUN && flush != ZU_FLUSH && flush != ZU_FINISH) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    return ZU_OK;
}

/* Resolves a codec to a registered vtable that supports `direction`. */
static zu_status zu_int_resolve(zu_codec codec, uint32_t direction,
                                const zu_codec_vtable **out)
{
    const zu_codec_vtable *v = zu_int_registry_lookup(codec);
    if (v == NULL) {
        return ZU_ERR_UNSUPPORTED;
    }
    if (!(v->flags & direction)) {
        return ZU_ERR_UNSUPPORTED;
    }
    *out = v;
    return ZU_OK;
}

/* A level the codec did not advertise is rejected here, once, rather than in
   every codec. ZU_LEVEL_DEFAULT always passes. */
static zu_status zu_int_check_level(const zu_codec_vtable *v, int32_t level)
{
    if (level == ZU_LEVEL_DEFAULT) {
        return ZU_OK;
    }
    /* All three zero means the codec has no level axis; only its default is
       acceptable, and only spelled as ZU_LEVEL_DEFAULT or that value. */
    if (v->level_min == 0 && v->level_max == 0 && v->level_default == 0) {
        return (level == 0) ? ZU_OK : ZU_ERR_INVALID_ARGUMENT;
    }
    if (level < v->level_min || level > v->level_max) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    return ZU_OK;
}

/* -- encoder ------------------------------------------------------------- */

zu_status zu_encoder_new(zu_encoder **out, const zu_encoder_opts *opts)
{
    if (out == NULL || opts == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (opts->struct_size < sizeof(zu_encoder_opts)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }

    const zu_codec_vtable *v = NULL;
    zu_status st = zu_int_resolve(opts->codec, ZU_CAN_ENCODE, &v);
    if (st != ZU_OK) {
        return st;
    }
    st = zu_int_check_level(v, opts->level);
    if (st != ZU_OK) {
        return st;
    }

    zu_encoder *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return ZU_ERR_MEMORY;
    }
    e->vtable = v;
    e->opts   = *opts;

    st = v->encoder_new(&e->state, opts);
    if (st != ZU_OK) {
        free(e);
        return st;
    }
    *out = e;
    return ZU_OK;
}

zu_status zu_encoder_process(zu_encoder *e, zu_buffer *buf, zu_flush flush)
{
    if (e == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    zu_status st = zu_int_check_buffer(buf);
    if (st != ZU_OK) {
        return st;
    }
    st = zu_int_check_flush(flush);
    if (st != ZU_OK) {
        return st;
    }
    if (e->finished) {
        /* Feeding a terminated stream is a caller bug, not a data error. */
        return (buf->src_pos < buf->src_size)
             ? ZU_ERR_INVALID_ARGUMENT : ZU_STREAM_END;
    }
    if (flush == ZU_FLUSH && !(e->vtable->flags & ZU_CAN_FLUSH)) {
        return ZU_ERR_UNSUPPORTED;
    }

    st = e->vtable->encoder_process(e->state, buf, flush);
    if (st == ZU_STREAM_END) {
        e->finished = 1;
    }
    return st;
}

zu_status zu_encoder_reset(zu_encoder *e, const zu_encoder_opts *opts)
{
    if (e == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    const zu_encoder_opts *use = (opts != NULL) ? opts : &e->opts;
    if (use->struct_size < sizeof(zu_encoder_opts)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    /* Reset re-parameterises one codec's stream; it does not switch codecs.
       Swapping codecs means a new handle, so the vtable stays fixed. */
    if (use->codec != e->opts.codec) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    zu_status st = zu_int_check_level(e->vtable, use->level);
    if (st != ZU_OK) {
        return st;
    }
    if (e->vtable->encoder_reset == NULL) {
        return ZU_ERR_UNSUPPORTED;
    }
    st = e->vtable->encoder_reset(e->state, use);
    if (st != ZU_OK) {
        return st;
    }
    e->opts     = *use;
    e->finished = 0;
    return ZU_OK;
}

void zu_encoder_free(zu_encoder *e)
{
    if (e == NULL) {
        return;             /* free(NULL) semantics: always safe */
    }
    if (e->vtable != NULL && e->vtable->encoder_free != NULL) {
        e->vtable->encoder_free(e->state);
    }
    free(e);
}

/* -- decoder ------------------------------------------------------------- */

zu_status zu_decoder_new(zu_decoder **out, const zu_decoder_opts *opts)
{
    if (out == NULL || opts == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (opts->struct_size < sizeof(zu_decoder_opts)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }

    const zu_codec_vtable *v = NULL;
    zu_status st = zu_int_resolve(opts->codec, ZU_CAN_DECODE, &v);
    if (st != ZU_OK) {
        return st;
    }

    zu_decoder *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return ZU_ERR_MEMORY;
    }
    d->vtable = v;
    d->opts   = *opts;

    st = v->decoder_new(&d->state, opts);
    if (st != ZU_OK) {
        free(d);
        return st;
    }
    *out = d;
    return ZU_OK;
}

/* Enforces max_output by *shrinking the window the codec is given*, rather
   than by checking afterwards. A codec physically cannot write past the cap,
   so enforcement does not depend on the codec being well-behaved -- which is
   the whole point of putting limits in the driver. */
zu_status zu_decoder_process(zu_decoder *d, zu_buffer *buf, zu_flush flush)
{
    if (d == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    zu_status st = zu_int_check_buffer(buf);
    if (st != ZU_OK) {
        return st;
    }
    st = zu_int_check_flush(flush);
    if (st != ZU_OK) {
        return st;
    }
    if (d->finished) {
        return (buf->src_pos < buf->src_size)
             ? ZU_ERR_TRAILING : ZU_STREAM_END;
    }
    if (flush == ZU_FLUSH && !(d->vtable->flags & ZU_CAN_FLUSH)) {
        return ZU_ERR_UNSUPPORTED;
    }

    const size_t real_dst_size = buf->dst_size;
    size_t       allowance     = real_dst_size - buf->dst_pos;
    int          capped        = 0;

    if (d->opts.max_output != 0) {
        /* Note that reaching the cap exactly is not itself an error: a
           stream whose output is exactly max_output bytes still needs one
           more call to observe ZU_FINISH and terminate. So the allowance is
           allowed to be 0 here, and it is the codec asking for *more* room
           that distinguishes "finished, exactly at the cap" from "would have
           exceeded the cap". */
        uint64_t remaining = d->opts.max_output - d->total_out;
        if (remaining < (uint64_t) allowance) {
            allowance = (size_t) remaining;
            capped    = 1;
            buf->dst_size = buf->dst_pos + allowance;
        }
    }

    const size_t before_in  = buf->src_pos;
    const size_t before_out = buf->dst_pos;

    st = d->vtable->decoder_process(d->state, buf, flush);

    const size_t used_in  = buf->src_pos - before_in;
    const size_t produced = buf->dst_pos - before_out;

    if (capped) {
        buf->dst_size = real_dst_size;     /* restore the caller's view */
    }

    d->total_in  += (uint64_t) used_in;
    d->total_out += (uint64_t) produced;

    if (st == ZU_STREAM_END) {
        d->finished = 1;
    }

    /* The codec filled our shrunken window and still wants room: it is the
       cap, not the caller's buffer, that stopped it. */
    if (capped && st == ZU_NEED_OUTPUT && produced == allowance) {
        return ZU_ERR_OUTPUT_LIMIT;
    }
    /* Input left over with the allowance spent is the same situation seen
       from the other side, for codecs that report NEED_INPUT rather than
       NEED_OUTPUT when they stall. */
    if (capped && allowance == 0 && st != ZU_STREAM_END &&
        buf->src_pos < buf->src_size) {
        return ZU_ERR_OUTPUT_LIMIT;
    }
    if (d->opts.max_output != 0 && d->total_out > d->opts.max_output) {
        return ZU_ERR_OUTPUT_LIMIT;        /* belt and braces */
    }

    /* Ratio is a policy check, not a memory bound -- max_output already
       bounds memory -- so it is checked after the fact. It is off by
       default because legitimately compressible data routinely exceeds any
       safe-looking threshold (design 20). */
    if (st != ZU_ERR_OUTPUT_LIMIT && d->opts.max_ratio != 0 && d->total_out > 0) {
        uint64_t allowed;
        if (d->total_in == 0) {
            allowed = 0;                   /* output from no input at all */
        } else if (d->total_in > UINT64_MAX / d->opts.max_ratio) {
            allowed = UINT64_MAX;          /* cannot overflow into a pass */
        } else {
            allowed = d->total_in * d->opts.max_ratio;
        }
        if (d->total_out > allowed) {
            return ZU_ERR_RATIO_LIMIT;
        }
    }

    return st;
}

zu_status zu_decoder_reset(zu_decoder *d, const zu_decoder_opts *opts)
{
    if (d == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    const zu_decoder_opts *use = (opts != NULL) ? opts : &d->opts;
    if (use->struct_size < sizeof(zu_decoder_opts)) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (use->codec != d->opts.codec) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (d->vtable->decoder_reset == NULL) {
        return ZU_ERR_UNSUPPORTED;
    }
    zu_status st = d->vtable->decoder_reset(d->state, use);
    if (st != ZU_OK) {
        return st;
    }
    d->opts      = *use;
    d->finished  = 0;
    /* Counters are per-stream, so a reset stream starts a fresh budget.
       Carrying them over would make the second message on a connection fail
       a limit the first one had already spent. */
    d->total_in  = 0;
    d->total_out = 0;
    return ZU_OK;
}

void zu_decoder_free(zu_decoder *d)
{
    if (d == NULL) {
        return;
    }
    if (d->vtable != NULL && d->vtable->decoder_free != NULL) {
        d->vtable->decoder_free(d->state);
    }
    free(d);
}
