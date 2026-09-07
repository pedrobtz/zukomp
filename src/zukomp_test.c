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
    r.level      = (Rf_isNull(r_level) || Rf_asInteger(r_level) == NA_INTEGER)
                 ? ZU_LEVEL_DEFAULT : (int32_t) Rf_asInteger(r_level);
    r.max_output = (uint64_t) Rf_asReal(r_max_output);
    r.max_ratio  = (uint32_t) Rf_asInteger(r_max_ratio);
    r.in_chunk   = (size_t) Rf_asReal(r_in_chunk);
    r.out_chunk  = (size_t) Rf_asReal(r_out_chunk);
    r.flush_every = (uint64_t) Rf_asReal(r_flush_every);
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
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, n));
    for (int i = 0; i < n; i++) {
        INTEGER(out)[i] = (int) values[i];
        SET_STRING_ELT(nms, i, Rf_mkChar(names[i]));
    }
    Rf_setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}
