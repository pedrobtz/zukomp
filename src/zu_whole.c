/* The whole-buffer drive loop.
 *
 * komp_compress(), komp_decompress() and the zu_test_stream() harness all
 * come through here. That is the point: the roadmap's "no second code path"
 * is not a style preference but the reason the chunk-boundary sweeps mean
 * anything. If the whole-buffer functions had their own loop, every sweep
 * would be testing code the users never run.
 *
 * Design 13 governs this file. The output sink is malloc'd and owned by an
 * R external pointer with a finalizer, so R reclaims it if anything
 * longjmps; nothing here calls Rf_error() while holding a buffer; and
 * R_CheckUserInterrupt() is called in the loop, because decompressing a
 * gigabyte must be interruptible. */
#include <R.h>
#include <Rinternals.h>

#include "zu_rglue.h"

/* How often to check for a user interrupt. Often enough that Ctrl-C feels
   immediate, rarely enough not to matter. */
#define ZU_INT_INTERRUPT_EVERY 64

/* The sink is malloc'd, and R_CheckUserInterrupt() in the drive loop
   longjmps straight past any free(). So it is owned by an external pointer
   with a finalizer for its whole life, exactly as the stream handles are --
   design 13 rule 3.
 *
   The address changes under realloc, so the external pointer is updated on
   every growth. Getting that wrong leaks the new block and frees the old
   one, so it happens in the one place that can move the buffer. */
/* Live sink allocations, for the leak test.
 *
 * The existing leak tests watch R's Vcells, which saw the old R_alloc sink
 * and cannot see a malloc'd one -- so moving to realloc would have made a
 * leak in exactly this buffer invisible to the suite that exists to catch
 * it. This counter is what keeps that honest. Touched only under R's
 * single-threaded evaluation, like every R_alloc call it replaces. */
static long zu_int_outbuf_live = 0;

long zu_int_outbuf_live_count(void)
{
    return zu_int_outbuf_live;
}

static void zu_int_outbuf_finalizer(SEXP ptr)
{
    void *p = R_ExternalPtrAddr(ptr);
    if (p != NULL) {
        free(p);
        zu_int_outbuf_live--;
        R_ClearExternalPtr(ptr);
    }
}

SEXP zu_int_outbuf_owner(zu_int_outbuf *o)
{
    SEXP ptr = PROTECT(R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(ptr, zu_int_outbuf_finalizer, TRUE);
    o->owner = (void *) ptr;
    o->buf   = NULL;
    o->size  = 0;
    o->used  = 0;
    UNPROTECT(1);
    return ptr;
}

/* Normal exit: free eagerly rather than waiting for a gc, clearing the
   external pointer first so the finalizer cannot free it a second time. */
void zu_int_outbuf_release(zu_int_outbuf *o)
{
    if (o->owner != NULL) {
        R_ClearExternalPtr((SEXP) o->owner);
    }
    if (o->buf != NULL) {
        free(o->buf);
        zu_int_outbuf_live--;
    }
    o->buf  = NULL;
    o->size = 0;
}

static zu_status zu_int_reserve(zu_int_outbuf *o, size_t extra)
{
    if (o->owner == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;   /* see zu_int_outbuf_owner() */
    }
    size_t needed;
    zu_status st = zu_int_add(o->used, extra, &needed);
    if (st != ZU_OK) {
        return st;
    }
    if (needed <= o->size) {
        return ZU_OK;
    }
    /* zu_int_grow() takes "how much more than `current`", so the shortfall
       is what it is asked for -- passing `extra` would ask for room the
       buffer already has. */
    size_t next;
    st = zu_int_grow(o->size, needed - o->size, &next);
    if (st != ZU_OK) {
        return st;
    }
    /* realloc, not allocate-and-copy: R_alloc cannot resize, so the old
       block used to stay on the vmax stack until the enclosing .Call
       returned and several superseded blocks were live at once -- decoding
       64 MB peaked at ~199 MB, a 3.1x ratio. realloc releases the old block
       as it goes, and frequently grows a large block in place. */
    uint8_t *bigger = (uint8_t *) realloc(o->buf, next);
    if (bigger == NULL) {
        /* o->buf is still valid and still owned: realloc leaves the
           original block untouched when it fails. */
        return ZU_ERR_MEMORY;
    }
    if (o->buf == NULL) {
        zu_int_outbuf_live++;       /* first allocation for this sink */
    }
    /* The established idiom is memset() followed by zu_int_outbuf_owner();
       a caller who does the first and forgets the second would otherwise
       segfault inside R_SetExternalPtrAddr on the first growth. */
    o->buf  = bigger;
    o->size = next;
    R_SetExternalPtrAddr((SEXP) o->owner, bigger);
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

    /* Deliberately NOT pre-sized from zu_compress_bound() on the encode
       path. The bound is roughly the input size, so reserving it would cost
       1x the input for every compression -- while the growth path, now that
       realloc releases as it goes, peaks at roughly the *output* size, which
       for compressible data is the whole point. Pre-sizing would only pay
       for incompressible input and would lose badly everywhere else. It is
       also not an upper bound once ZU_FLUSH is in play, since every sync
       flush inserts an empty stored block. */

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

        /* Note that ZU_FINISH is only ever seen alongside an *empty* buffer:
           the refill above resets src_pos to 0, so this is false whenever
           bytes were just handed over. A codec therefore cannot tell "here
           are the final bytes" from "here are some bytes", which is why
           gzip's member probe has to consume a trailing 0x1F speculatively
           rather than recognising it as the last byte.
         *
           Sending FINISH with the last bytes would fix that, and was tried:
           it also changes where the DEFLATE body reports ZU_ERR_TRUNCATED
           versus ZU_ERR_INVALID_DATA, which design 7 pins deliberately and
           test-truncation.R checks position by position. That trade belongs
           in its own change, not smuggled in behind a probe fix. */
        const int last = (fed >= r->n);
        zu_flush flush = last ? ZU_FINISH : ZU_RUN;
        if (!last && r->flush_every > 0 &&
            ((calls + 1) % r->flush_every) == 0) {
            flush = ZU_FLUSH;
        }

        st = zu_int_reserve(out, r->out_chunk);
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
            /* Longjmps out. Safe here only because both the output sink and
               the stream handles are owned by external pointers with
               finalizers: nothing below this line is reached on an
               interrupt, so anything held in a bare local would leak. */
            R_CheckUserInterrupt();
        }
    }

    /* What the codec actually took, as opposed to what was offered. The
       difference is the trailing run, and keeping it exact is the invariant
       that lets a caller tell "stream complete" from "stream complete, junk
       follows" -- and resume a connection at the right byte. */
    out->consumed = fed - (buf.src_size - buf.src_pos);

    /* Normal exit: free eagerly rather than waiting for a gc, and clear the
       pointer so the finalizer cannot free it a second time. */
    R_ClearExternalPtr(guard);
    UNPROTECT(1);
    zu_encoder_free(enc);
    zu_decoder_free(dec);
    return st;
}
