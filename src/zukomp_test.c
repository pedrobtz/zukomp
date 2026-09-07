/* Test-only entry points. Unexported, prefixed zukomp_test_, and compiled
 * always so the C stream driver is exercised from the very stage that
 * introduces it.
 *
 * The R streaming API is phase 2, but chunk-boundary correctness must be
 * testable from Stage 4: it is precisely the property zuhttp depends on and
 * the one that rots silently. This harness drives the driver at
 * caller-chosen input and output chunk sizes, which is what makes the
 * boundary sweeps possible years before komp_stream_new() exists.
 *
 * Design 13 rule 1 is binding here: nothing in this file calls Rf_error()
 * while holding a buffer. Entry points return a (status, bytes) pair and R
 * decides whether that is a condition. */
#include <R.h>
#include <Rinternals.h>

#include "zu_internal.h"

/* Growing output for the harness lives on R_alloc, bracketed by
   vmaxget/vmaxset, so R reclaims it even if something later longjmps. */
typedef struct {
    char   *vmax;
    uint8_t *buf;
    size_t   size;
    size_t   used;
} zu_int_outbuf;

static zu_status zu_int_outbuf_reserve(zu_int_outbuf *o, size_t extra, size_t cap)
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

static SEXP zu_int_result(zu_status status, const uint8_t *bytes, size_t n)
{
    SEXP out = PROTECT(Rf_allocVector(VECSXP, 2));
    SET_VECTOR_ELT(out, 0, Rf_ScalarInteger((int) status));
    SEXP raw = Rf_allocVector(RAWSXP, (R_xlen_t) n);
    SET_VECTOR_ELT(out, 1, raw);
    if (n > 0) {
        memcpy(RAW(raw), bytes, n);
    }
    SEXP names = Rf_allocVector(STRSXP, 2);
    Rf_setAttrib(out, R_NamesSymbol, names);
    SET_STRING_ELT(names, 0, Rf_mkChar("status"));
    SET_STRING_ELT(names, 1, Rf_mkChar("bytes"));
    UNPROTECT(1);
    return out;
}

/* Drives the stream driver over `bytes`, feeding at most in_chunk input and
   offering at most out_chunk output space per call, so that every awkward
   boundary a real caller might hit is reachable from a test. */
SEXP zukomp_test_stream(SEXP r_bytes, SEXP r_codec, SEXP r_encode,
                        SEXP r_in_chunk, SEXP r_out_chunk,
                        SEXP r_max_output, SEXP r_max_ratio,
                        SEXP r_flush_every, SEXP r_level,
                        SEXP r_reject_trailing, SEXP r_concat_members)
{
    const uint8_t *src = (const uint8_t *) RAW(r_bytes);
    const size_t   n   = (size_t) Rf_xlength(r_bytes);

    zu_codec codec = zu_codec_lookup(CHAR(STRING_ELT(r_codec, 0)));
    const int encode      = Rf_asLogical(r_encode) == TRUE;
    const size_t in_chunk  = (size_t) Rf_asReal(r_in_chunk);
    const size_t out_chunk = (size_t) Rf_asReal(r_out_chunk);
    const uint64_t max_output = (uint64_t) Rf_asReal(r_max_output);
    const uint32_t max_ratio  = (uint32_t) Rf_asInteger(r_max_ratio);
    const R_xlen_t flush_every = (R_xlen_t) Rf_asReal(r_flush_every);
    const int32_t level = (Rf_isNull(r_level) || Rf_asInteger(r_level) == NA_INTEGER)
                        ? ZU_LEVEL_DEFAULT : (int32_t) Rf_asInteger(r_level);

    if (codec == ZU_CODEC_NONE) {
        return zu_int_result(ZU_ERR_UNSUPPORTED, NULL, 0);
    }
    if (in_chunk == 0 || out_chunk == 0) {
        return zu_int_result(ZU_ERR_INVALID_ARGUMENT, NULL, 0);
    }

    zu_int_outbuf out;
    out.vmax = vmaxget();
    out.buf  = NULL;
    out.size = 0;
    out.used = 0;

    zu_encoder *enc = NULL;
    zu_decoder *dec = NULL;
    zu_status   st;

    if (encode) {
        zu_encoder_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.struct_size = (uint32_t) sizeof(opts);
        opts.codec = codec;
        opts.level = level;
        st = zu_encoder_new(&enc, &opts);
    } else {
        zu_decoder_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.struct_size = (uint32_t) sizeof(opts);
        opts.codec = codec;
        opts.max_output = max_output;
        opts.max_ratio  = max_ratio;
        if (Rf_asLogical(r_reject_trailing) == TRUE) {
            opts.flags |= ZU_DEC_REJECT_TRAILING;
        }
        if (Rf_asLogical(r_concat_members) == TRUE) {
            opts.flags |= ZU_DEC_CONCAT_MEMBERS;
        }
        st = zu_decoder_new(&dec, &opts);
    }
    if (st != ZU_OK) {
        vmaxset(out.vmax);
        return zu_int_result(st, NULL, 0);
    }

    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));

    size_t   fed   = 0;      /* input handed to the stream so far */
    R_xlen_t calls = 0;
    st = ZU_OK;

    for (;;) {
        /* Present the next input slice only once the previous one is spent,
           so src_pos genuinely walks a chunk at a time. */
        if (buf.src_pos == buf.src_size && fed < n) {
            size_t take = n - fed;
            if (take > in_chunk) {
                take = in_chunk;
            }
            buf.src      = src + fed;
            buf.src_size = take;
            buf.src_pos  = 0;
            fed += take;
        }

        const int last = (fed >= n) && (buf.src_pos == buf.src_size);
        zu_flush flush = last ? ZU_FINISH : ZU_RUN;
        if (!last && flush_every > 0 && ((calls + 1) % flush_every) == 0) {
            flush = ZU_FLUSH;
        }

        st = zu_int_outbuf_reserve(&out, out_chunk, 0);
        if (st != ZU_OK) {
            break;
        }
        buf.dst      = out.buf + out.used;
        buf.dst_size = out_chunk;
        buf.dst_pos  = 0;

        st = encode ? zu_encoder_process(enc, &buf, flush)
                    : zu_decoder_process(dec, &buf, flush);

        out.used += buf.dst_pos;
        calls++;

        if (st == ZU_STREAM_END) {
            break;
        }
        if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
            break;                      /* a real error */
        }
        /* No progress and nothing left to give: the codec is stuck, and
           looping forever would hang the R session rather than fail a test. */
        if (buf.dst_pos == 0 && last && st == ZU_NEED_INPUT) {
            st = ZU_ERR_INTERNAL;
            break;
        }
        if (calls > 0 && (size_t) calls > (n + 16) * 8 + 4096) {
            st = ZU_ERR_INTERNAL;       /* runaway guard */
            break;
        }
    }

    zu_encoder_free(enc);
    zu_decoder_free(dec);

    SEXP result = zu_int_result(st, out.buf, out.used);
    vmaxset(out.vmax);
    return result;
}

/* Asks zu_int_grow() to grow a buffer that is already near SIZE_MAX, which
   is unreachable through any real input but is exactly the arithmetic a
   decompression bomb is trying to provoke. */
SEXP zukomp_test_grow(SEXP r_near_size_max)
{
    const int near = Rf_asLogical(r_near_size_max) == TRUE;
    size_t current = near ? (SIZE_MAX - 16) : 1024;
    size_t out = 0;
    zu_status st = zu_int_grow(current, 4096, 0, &out);
    return zu_int_result(st, NULL, 0);
}

/* The zu_status enum as a named integer vector, so R maps statuses to
   condition classes by name instead of hardcoding enum ordinals. */
SEXP zukomp_status_codes(void)
{
    static const char *names[] = {
        "ZU_OK", "ZU_NEED_INPUT", "ZU_NEED_OUTPUT", "ZU_STREAM_END",
        "ZU_ERR_INVALID_ARGUMENT", "ZU_ERR_UNSUPPORTED", "ZU_ERR_INVALID_DATA",
        "ZU_ERR_TRUNCATED", "ZU_ERR_CHECKSUM", "ZU_ERR_TRAILING",
        "ZU_ERR_MEMORY", "ZU_ERR_OUTPUT_LIMIT", "ZU_ERR_RATIO_LIMIT",
        "ZU_ERR_INTERNAL"
    };
    static const zu_status values[] = {
        ZU_OK, ZU_NEED_INPUT, ZU_NEED_OUTPUT, ZU_STREAM_END,
        ZU_ERR_INVALID_ARGUMENT, ZU_ERR_UNSUPPORTED, ZU_ERR_INVALID_DATA,
        ZU_ERR_TRUNCATED, ZU_ERR_CHECKSUM, ZU_ERR_TRAILING,
        ZU_ERR_MEMORY, ZU_ERR_OUTPUT_LIMIT, ZU_ERR_RATIO_LIMIT,
        ZU_ERR_INTERNAL
    };
    const int n = (int) (sizeof(values) / sizeof(values[0]));

    SEXP out = PROTECT(Rf_allocVector(INTSXP, n));
    SEXP nms = Rf_allocVector(STRSXP, n);
    Rf_setAttrib(out, R_NamesSymbol, nms);
    for (int i = 0; i < n; i++) {
        INTEGER(out)[i] = (int) values[i];
        SET_STRING_ELT(nms, i, Rf_mkChar(names[i]));
    }
    UNPROTECT(1);
    return out;
}
