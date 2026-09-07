/* The whole-buffer drive loop.
 *
 * komp_compress(), komp_decompress() and the zu_test_stream() harness all
 * come through here. That is the point: the roadmap's "no second code path"
 * is not a style preference but the reason the chunk-boundary sweeps mean
 * anything. If the whole-buffer functions had their own loop, every sweep
 * would be testing code the users never run.
 *
 * Design 13 governs this file. Output grows on R_alloc bracketed by
 * vmaxget/vmaxset, so R reclaims it if anything longjmps; nothing here
 * calls Rf_error() while holding a buffer; and R_CheckUserInterrupt() is
 * called in the loop, because decompressing a gigabyte must be
 * interruptible. */
#include <R.h>
#include <Rinternals.h>

#include "zu_rglue.h"

/* How often to check for a user interrupt. Often enough that Ctrl-C feels
   immediate, rarely enough not to matter. */
#define ZU_INT_INTERRUPT_EVERY 64

static zu_status zu_int_reserve(zu_int_outbuf *o, size_t extra, size_t cap)
{
    size_t needed;
    zu_status st = zu_int_add(o->used, extra, &needed);
    if (st != ZU_OK) {
        return st;
    }
    if (needed <= o->size) {
        return ZU_OK;
    }
    size_t next;
    st = zu_int_grow(o->size, extra, cap, &next);
    if (st != ZU_OK) {
        return st;
    }
    /* R_alloc cannot resize, so grow by allocating and copying. The old
       block stays on the vmax stack until the enclosing .Call returns,
       which is wasteful but safe -- and the doubling in zu_int_grow keeps
       the number of copies logarithmic. */
    uint8_t *bigger = (uint8_t *) R_alloc(next, 1);
    if (bigger == NULL) {
        return ZU_ERR_MEMORY;
    }
    if (o->used > 0) {
        memcpy(bigger, o->buf, o->used);
    }
    o->buf  = bigger;
    o->size = next;
    return ZU_OK;
}

/* Stream handles are malloc'd, and R_CheckUserInterrupt() longjmps straight
   past any free() below it. So the handle is owned by an external pointer
   with a finalizer for the duration: on an interrupt the pointer becomes
   garbage and the finalizer frees the stream, instead of it leaking once
   per interrupted decompression.
 *
 * This is design 13 rule 3, which says building the invariant now costs
 * nothing. It cost one bug: the first version of this loop called
 * R_CheckUserInterrupt() with the handles held in bare locals. */
static void zu_int_encoder_finalizer(SEXP ptr)
{
    zu_encoder *e = (zu_encoder *) R_ExternalPtrAddr(ptr);
    if (e != NULL) {
        zu_encoder_free(e);
        R_ClearExternalPtr(ptr);
    }
}

static void zu_int_decoder_finalizer(SEXP ptr)
{
    zu_decoder *d = (zu_decoder *) R_ExternalPtrAddr(ptr);
    if (d != NULL) {
        zu_decoder_free(d);
        R_ClearExternalPtr(ptr);
    }
}

zu_status zu_int_run_whole(const zu_int_run_opts *r, zu_int_outbuf *out)
{
    if (r == NULL || out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (r->in_chunk == 0 || r->out_chunk == 0) {
        return ZU_ERR_INVALID_ARGUMENT;
    }

    zu_encoder *enc = NULL;
    zu_decoder *dec = NULL;
    zu_status   st;

    if (r->encode) {
        zu_encoder_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.struct_size = (uint32_t) sizeof(opts);
        opts.codec = r->codec;
        opts.level = r->level;
        st = zu_encoder_new(&enc, &opts);
    } else {
        zu_decoder_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.struct_size = (uint32_t) sizeof(opts);
        opts.codec      = r->codec;
        opts.max_output = r->max_output;
        opts.max_ratio  = r->max_ratio;
        opts.flags      = r->dec_flags;
        st = zu_decoder_new(&dec, &opts);
    }
    if (st != ZU_OK) {
        return st;
    }

    /* Hand ownership to R before entering a loop that can longjmp. */
    SEXP guard = PROTECT(R_MakeExternalPtr(r->encode ? (void *) enc : (void *) dec,
                                           R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(guard,
                           r->encode ? zu_int_encoder_finalizer
                                     : zu_int_decoder_finalizer,
                           TRUE);

    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));

    size_t   fed   = 0;
    uint64_t calls = 0;
    st = ZU_OK;

    for (;;) {
        /* Hand over the next input slice only once the previous one is
           spent, so src_pos really does walk a chunk at a time and the
           sweeps exercise what they claim to. */
        if (buf.src_pos == buf.src_size && fed < r->n) {
            size_t take = r->n - fed;
            if (take > r->in_chunk) {
                take = r->in_chunk;
            }
            buf.src      = r->src + fed;
            buf.src_size = take;
            buf.src_pos  = 0;
            fed += take;
        }

        const int last = (fed >= r->n) && (buf.src_pos == buf.src_size);
        zu_flush flush = last ? ZU_FINISH : ZU_RUN;
        if (!last && r->flush_every > 0 &&
            ((calls + 1) % r->flush_every) == 0) {
            flush = ZU_FLUSH;
        }

        st = zu_int_reserve(out, r->out_chunk, r->buffer_cap);
        if (st != ZU_OK) {
            break;
        }
        buf.dst      = out->buf + out->used;
        buf.dst_size = r->out_chunk;
        buf.dst_pos  = 0;

        st = r->encode ? zu_encoder_process(enc, &buf, flush)
                       : zu_decoder_process(dec, &buf, flush);

        out->used += buf.dst_pos;
        calls++;

        if (st == ZU_STREAM_END) {
            break;
        }
        if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
            break;
        }
        /* Nothing moved and there is nothing left to give: the codec is
           stuck. Failing beats hanging the R session in a loop no
           interrupt-free path can escape. */
        if (buf.dst_pos == 0 && last && st == ZU_NEED_INPUT) {
            st = ZU_ERR_INTERNAL;
            break;
        }

        if ((calls % ZU_INT_INTERRUPT_EVERY) == 0) {
            /* Longjmps out. Safe here only because the output buffer is on
               R_alloc and the stream handles are freed by the caller's
               unwinding -- see the note in the callers. */
            R_CheckUserInterrupt();
        }
    }

    /* Normal exit: free eagerly rather than waiting for a gc, and clear the
       pointer so the finalizer cannot free it a second time. */
    R_ClearExternalPtr(guard);
    UNPROTECT(1);
    zu_encoder_free(enc);
    zu_decoder_free(dec);
    return st;
}
