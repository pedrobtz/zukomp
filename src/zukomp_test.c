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

#include "zu_rglue.h"

SEXP zu_int_result(zu_status status, const uint8_t *bytes, size_t n)
{
    SEXP out = PROTECT(Rf_allocVector(VECSXP, 2));
    SET_VECTOR_ELT(out, 0, Rf_ScalarInteger((int) status));
    SEXP raw = Rf_allocVector(RAWSXP, (R_xlen_t) n);
    SET_VECTOR_ELT(out, 1, raw);
    if (n > 0) {
        memcpy(RAW(raw), bytes, n);
    }
    /* PROTECT the names before setAttrib: between allocVector and the
       attribute actually being installed, nothing else is holding them. */
    SEXP names = PROTECT(Rf_allocVector(STRSXP, 2));
    SET_STRING_ELT(names, 0, Rf_mkChar("status"));
    SET_STRING_ELT(names, 1, Rf_mkChar("bytes"));
    Rf_setAttrib(out, R_NamesSymbol, names);
    UNPROTECT(2);
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
    zu_int_run_opts r;
    memset(&r, 0, sizeof(r));
    r.src        = (const uint8_t *) RAW(r_bytes);
    r.n          = (size_t) Rf_xlength(r_bytes);
    r.encode     = Rf_asLogical(r_encode) == TRUE;
    r.codec      = zu_codec_lookup(CHAR(STRING_ELT(r_codec, 0)));
    /* Every one of these goes through a narrowing helper. The harness is
       called from tests with deliberately awkward values, and a bare cast
       of NA, Inf or a negative double to size_t is undefined behaviour --
       the float-cast-overflow UBSan reports the sanitizer jobs exist to
       catch, and a wrapped chunk size then drives zu_int_reserve(). */
    if (zu_int_level_from_sexp(r_level, &r.level) != 0 ||
        zu_int_u64_from_real(Rf_asReal(r_max_output), &r.max_output) != 0 ||
        zu_int_u32_from_int(Rf_asInteger(r_max_ratio), &r.max_ratio) != 0 ||
        zu_int_size_from_real(Rf_asReal(r_in_chunk), &r.in_chunk) != 0 ||
        zu_int_size_from_real(Rf_asReal(r_out_chunk), &r.out_chunk) != 0 ||
        zu_int_u64_from_real(Rf_asReal(r_flush_every), &r.flush_every) != 0) {
        return zu_int_result(ZU_ERR_INVALID_ARGUMENT, NULL, 0);
    }
    if (Rf_asLogical(r_reject_trailing) == TRUE) {
        r.dec_flags |= ZU_DEC_REJECT_TRAILING;
    }
    if (Rf_asLogical(r_concat_members) == TRUE) {
        r.dec_flags |= ZU_DEC_CONCAT_MEMBERS;
    }

    if (r.codec == ZU_CODEC_NONE) {
        return zu_int_result(ZU_ERR_UNSUPPORTED, NULL, 0);
    }

    zu_int_outbuf out;
    memset(&out, 0, sizeof(out));
    out.vmax = vmaxget();

    zu_status st = zu_int_run_whole(&r, &out);

    SEXP result = zu_int_result(st, out.buf, out.used);
    vmaxset(out.vmax);
    return result;
}

/* zu_decompress_one() at a caller-chosen output capacity.
 *
 * The whole-buffer R API grows its own sink, so nothing else reaches the
 * one-shot ABI -- the entry point a consumer with a known Content-Length
 * actually calls -- and nothing else can offer it a zero-byte sink. A
 * zero-output stream into a zero-capacity sink is ZU_OK with nothing
 * written, not an output-limit error.
 */
SEXP zukomp_test_decompress_one(SEXP r_bytes, SEXP r_codec, SEXP r_cap)
{
    zu_codec codec = zu_codec_lookup(CHAR(STRING_ELT(r_codec, 0)));
    if (codec == ZU_CODEC_NONE) {
        return zu_int_result(ZU_ERR_UNSUPPORTED, NULL, 0);
    }
    size_t cap = 0;
    if (zu_int_size_from_real(Rf_asReal(r_cap), &cap) != 0) {
        return zu_int_result(ZU_ERR_INVALID_ARGUMENT, NULL, 0);
    }

    zu_decoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec = codec;
    opts.flags = ZU_DEC_REJECT_TRAILING | ZU_DEC_CONCAT_MEMBERS;

    char    *vmax = vmaxget();
    uint8_t *dst  = (cap == 0) ? NULL : (uint8_t *) R_alloc(cap, 1);
    size_t   written = 0;

    zu_status st = zu_decompress_one(&opts,
                                     (const uint8_t *) RAW(r_bytes),
                                     (size_t) Rf_xlength(r_bytes),
                                     dst, cap, &written);
    SEXP out = zu_int_result(st, dst, written);
    vmaxset(vmax);
    return out;
}

/* Encodes twice through one encoder handle, with zu_encoder_reset() and a
 * second level in between, and returns the second stream.
 *
 * zu_encoder_reset() is public ABI and is the shape zuhttp is told to adopt
 * -- one stream per keep-alive connection, reset per message -- but nothing
 * in the R API reaches it, so without this entry point it is untested. The
 * bug it pins down: mz_deflateReset() re-uses the flags baked in at init, so
 * the new level reached the zlib header and not the payload.
 */
SEXP zukomp_test_encoder_reset(SEXP r_bytes, SEXP r_codec,
                               SEXP r_level1, SEXP r_level2)
{
    zu_codec codec = zu_codec_lookup(CHAR(STRING_ELT(r_codec, 0)));
    if (codec == ZU_CODEC_NONE) {
        return zu_int_result(ZU_ERR_UNSUPPORTED, NULL, 0);
    }

    zu_encoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec = codec;
    if (zu_int_level_from_sexp(r_level1, &opts.level) != 0) {
        return zu_int_result(ZU_ERR_INVALID_ARGUMENT, NULL, 0);
    }

    const uint8_t *src = (const uint8_t *) RAW(r_bytes);
    const size_t   n   = (size_t) Rf_xlength(r_bytes);

    size_t cap = 0;
    zu_status st = zu_compress_bound(codec, opts.level, n, &cap);
    if (st != ZU_OK) {
        return zu_int_result(st, NULL, 0);
    }

    zu_encoder *enc = NULL;
    st = zu_encoder_new(&enc, &opts);
    if (st != ZU_OK) {
        return zu_int_result(st, NULL, 0);
    }

    /* R_alloc, so a longjmp cannot leak it; nothing below raises anyway. */
    char *vmax = vmaxget();
    uint8_t *dst = (uint8_t *) R_alloc(cap == 0 ? 1 : cap, 1);

    for (int pass = 0; pass < 2 && st == ZU_OK; pass++) {
        if (pass == 1) {
            if (zu_int_level_from_sexp(r_level2, &opts.level) != 0) {
                st = ZU_ERR_INVALID_ARGUMENT;
                break;
            }
            st = zu_encoder_reset(enc, &opts);
            if (st != ZU_OK) {
                break;
            }
        }

        zu_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.src = src; buf.src_size = n;
        buf.dst = dst; buf.dst_size = cap;

        for (;;) {
            const size_t before_out = buf.dst_pos;
            const size_t before_in  = buf.src_pos;
            zu_status ps = zu_encoder_process(enc, &buf, ZU_FINISH);
            if (ps == ZU_STREAM_END) {
                st = ZU_OK;
                break;
            }
            if (ps != ZU_OK && ps != ZU_NEED_INPUT && ps != ZU_NEED_OUTPUT) {
                st = ps;
                break;
            }
            if (buf.dst_pos == before_out && buf.src_pos == before_in) {
                st = ZU_ERR_INTERNAL;   /* no progress; refuse to spin */
                break;
            }
        }

        if (pass == 1 || st != ZU_OK) {
            SEXP out = zu_int_result(st, dst, buf.dst_pos);
            zu_encoder_free(enc);
            vmaxset(vmax);
            return out;
        }
    }

    SEXP out = zu_int_result(st, NULL, 0);
    zu_encoder_free(enc);
    vmaxset(vmax);
    return out;
}

/* Asks zu_int_grow() to grow a buffer that is already near SIZE_MAX, which
   is unreachable through any real input but is exactly the arithmetic a
   decompression bomb is trying to provoke. */
SEXP zukomp_test_grow(SEXP r_near_size_max)
{
    const int near = Rf_asLogical(r_near_size_max) == TRUE;
    size_t current = near ? (SIZE_MAX - 16) : 1024;
    size_t out = 0;
    zu_status st = zu_int_grow(current, 4096, &out);
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
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, n));
    for (int i = 0; i < n; i++) {
        INTEGER(out)[i] = (int) values[i];
        SET_STRING_ELT(nms, i, Rf_mkChar(names[i]));
    }
    Rf_setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}
