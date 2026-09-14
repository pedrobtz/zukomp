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
    /* The sink is malloc'd and owned by this external pointer for as long
       as out.buf is read, so an interrupt inside the drive loop frees it
       instead of leaking it. */
    SEXP owner = PROTECT(zu_int_outbuf_owner(&out));

    zu_status st = zu_int_run_whole(&r, &out);

    SEXP result = PROTECT(zu_int_result(st, out.buf, out.used));
    SEXP cons = PROTECT(Rf_ScalarReal((double) out.consumed));
    Rf_setAttrib(result, Rf_install("consumed"), cons);
    zu_int_outbuf_release(&out);
    UNPROTECT(3);
    (void) owner;
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
            /* Free before allocating the result, not after: `out` would
               otherwise be an unprotected SEXP live across a call rchk must
               treat as allocating -- encoder_free is a vtable function
               pointer, so a third-party codec's could do anything. Freeing
               first needs no PROTECT and is what rchk reported here. */
            zu_encoder_free(enc);
            SEXP out = zu_int_result(st, dst, buf.dst_pos);
            vmaxset(vmax);
            return out;
        }
    }

    zu_encoder_free(enc);
    SEXP out = zu_int_result(st, NULL, 0);
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

/* zu_compress_bound() alone, with no compression.
 *
 * The R helper used to get this by running a whole zu_compress_one() and
 * reading the attribute off the result, which compressed the payload to
 * throw it away -- and raised a condition when the codec errored, from a
 * function documented as a pure query. */
SEXP zukomp_test_compress_bound(SEXP r_codec, SEXP r_level, SEXP r_n)
{
    zu_codec codec = zu_codec_lookup(CHAR(STRING_ELT(r_codec, 0)));
    if (codec == ZU_CODEC_NONE) {
        return zu_int_result(ZU_ERR_UNSUPPORTED, NULL, 0);
    }
    int32_t level = ZU_LEVEL_DEFAULT;
    if (zu_int_level_from_sexp(r_level, &level) != 0) {
        return zu_int_result(ZU_ERR_INVALID_ARGUMENT, NULL, 0);
    }
    size_t n = 0;
    if (zu_int_size_from_real(Rf_asReal(r_n), &n) != 0) {
        return zu_int_result(ZU_ERR_INVALID_ARGUMENT, NULL, 0);
    }

    size_t bound = 0;
    zu_status st = zu_compress_bound(codec, level, n, &bound);

    SEXP out = PROTECT(zu_int_result(st, NULL, 0));
    SEXP b   = PROTECT(Rf_ScalarReal((double) bound));
    Rf_setAttrib(out, Rf_install("bound"), b);
    UNPROTECT(2);
    return out;
}

/* Decodes two messages through one decoder handle with zu_decoder_reset()
 * in between, and returns both outputs concatenated.
 *
 * zu_decoder_reset() had no caller outside its own definition. It has more
 * state to get right than the encoder's: total_in/total_out and so every
 * limit budget, the wrapper state machine, the gzip header parser, and
 * miniz's own stream. The shape that matters is a keep-alive connection
 * decoding a second response body through the handle that decoded the
 * first.
 *
 * `max_output` applies to each message separately, which is the point: a
 * budget that carried over would make the second message on a connection
 * fail a limit the first one had already spent.
 *
 * The second message is decoded even when the first FAILS, which is the
 * whole point of the "a malformed response must not poison the connection"
 * case: guarding the loop on `st == ZU_OK` meant that test never reached the
 * reset at all and passed whether or not reset-after-error worked.
 *
 * Returns the two decoded messages back to back. `first_status` and
 * `first_n` come back as attributes so a test can tell which message failed
 * and where the second one's bytes begin.
 */
SEXP zukomp_test_decoder_reset(SEXP r_a, SEXP r_b, SEXP r_codec,
                               SEXP r_max_output)
{
    zu_codec codec = zu_codec_lookup(CHAR(STRING_ELT(r_codec, 0)));
    if (codec == ZU_CODEC_NONE) {
        return zu_int_result(ZU_ERR_UNSUPPORTED, NULL, 0);
    }

    zu_decoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec = codec;
    opts.flags = ZU_DEC_REJECT_TRAILING | ZU_DEC_CONCAT_MEMBERS;
    if (zu_int_u64_from_real(Rf_asReal(r_max_output), &opts.max_output) != 0) {
        return zu_int_result(ZU_ERR_INVALID_ARGUMENT, NULL, 0);
    }

    zu_decoder *dec = NULL;
    zu_status st = zu_decoder_new(&dec, &opts);
    if (st != ZU_OK) {
        return zu_int_result(st, NULL, 0);
    }

    /* The handle is malloc'd and nothing below longjmps, but R_alloc for the
       sink keeps design 13 rule 3 intact if that ever changes. */
    char  *vmax = vmaxget();
    size_t cap  = 1u << 20;
    uint8_t *dst = (uint8_t *) R_alloc(cap, 1);
    size_t   used = 0;

    SEXP msgs[2];
    msgs[0] = r_a;
    msgs[1] = r_b;

    zu_status first_st = ZU_OK;
    size_t    first_n  = 0;

    for (int i = 0; i < 2; i++) {
        if (i == 1) {
            first_st = st;
            first_n  = used;
            /* Deliberately not conditional on st: a decoder that hit an
               error must be usable again after a reset, or one malformed
               response poisons the connection for good. */
            st = zu_decoder_reset(dec, &opts);
            if (st != ZU_OK) {
                break;
            }
        }

        zu_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.src      = (const uint8_t *) RAW(msgs[i]);
        buf.src_size = (size_t) Rf_xlength(msgs[i]);

        for (;;) {
            /* ZU_ERR_INTERNAL, not ZU_ERR_OUTPUT_LIMIT: this is the harness's
               own fixed sink running out, and the limit tests assert on
               ZU_ERR_OUTPUT_LIMIT. Sharing the status would let a harness
               overflow masquerade as the security limit firing. */
            if (used >= cap) { st = ZU_ERR_INTERNAL; break; }
            buf.dst      = dst + used;
            buf.dst_size = cap - used;
            buf.dst_pos  = 0;

            zu_status ps = zu_decoder_process(dec, &buf, ZU_FINISH);
            used += buf.dst_pos;

            if (ps == ZU_STREAM_END) { st = ZU_OK; break; }
            if (ps != ZU_OK && ps != ZU_NEED_INPUT && ps != ZU_NEED_OUTPUT) {
                st = ps;
                break;
            }
            if (buf.dst_pos == 0 && ps == ZU_NEED_INPUT) {
                st = ZU_ERR_INTERNAL;   /* no progress; refuse to spin */
                break;
            }
        }
    }

    zu_decoder_free(dec);
    SEXP out = PROTECT(zu_int_result(st, dst, used));
    SEXP fs  = PROTECT(Rf_ScalarInteger((int) first_st));
    SEXP fn  = PROTECT(Rf_ScalarReal((double) first_n));
    Rf_setAttrib(out, Rf_install("first_status"), fs);
    Rf_setAttrib(out, Rf_install("first_n"), fn);
    UNPROTECT(3);
    vmaxset(vmax);
    return out;
}

/* Refuses a reset that names a different codec. Swapping codecs means a new
   handle: the vtable is fixed at zu_decoder_new() time and reset only
   re-parameterises one codec's stream. */
SEXP zukomp_test_decoder_reset_codec(SEXP r_from, SEXP r_to)
{
    zu_codec from = zu_codec_lookup(CHAR(STRING_ELT(r_from, 0)));
    zu_codec to   = zu_codec_lookup(CHAR(STRING_ELT(r_to, 0)));
    if (from == ZU_CODEC_NONE || to == ZU_CODEC_NONE) {
        return zu_int_result(ZU_ERR_UNSUPPORTED, NULL, 0);
    }

    zu_decoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec = from;

    zu_decoder *dec = NULL;
    zu_status st = zu_decoder_new(&dec, &opts);
    if (st != ZU_OK) {
        return zu_int_result(st, NULL, 0);
    }

    opts.codec = to;
    st = zu_decoder_reset(dec, &opts);
    zu_decoder_free(dec);
    return zu_int_result(st, NULL, 0);
}

/* zu_compress_bound() and zu_compress_one() at a caller-chosen capacity.
 *
 * The compress half of the one-shot ABI ran only against the consumer
 * package's xor5a until now -- a codec with no wrapper, no expansion and
 * bound(n) == n, which is the one shape that cannot catch a bound that
 * forgets a header, a trailer or stored-block overhead. The codecs where
 * the bound is closest to wrong are the wrapping ones on incompressible
 * input, so drive all four from here.
 *
 * `cap_delta` adjusts the capacity relative to the bound, so a test can ask
 * for exactly the bound (0), one byte less than the bound (-1), or one less
 * than the bytes actually needed.
 *
 * Returns the compressed bytes; `bound` comes back as an attribute so a
 * test can assert the bound itself, not only that it was large enough.
 */
SEXP zukomp_test_compress_one(SEXP r_bytes, SEXP r_codec, SEXP r_level,
                              SEXP r_cap_delta)
{
    zu_codec codec = zu_codec_lookup(CHAR(STRING_ELT(r_codec, 0)));
    if (codec == ZU_CODEC_NONE) {
        return zu_int_result(ZU_ERR_UNSUPPORTED, NULL, 0);
    }

    zu_encoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec = codec;
    if (zu_int_level_from_sexp(r_level, &opts.level) != 0) {
        return zu_int_result(ZU_ERR_INVALID_ARGUMENT, NULL, 0);
    }

    const uint8_t *src = (const uint8_t *) RAW(r_bytes);
    const size_t   n   = (size_t) Rf_xlength(r_bytes);

    size_t bound = 0;
    zu_status st = zu_compress_bound(codec, opts.level, n, &bound);
    if (st != ZU_OK) {
        return zu_int_result(st, NULL, 0);
    }

    /* Narrow through the same checked path as every other size that
       crosses this boundary. Casting a non-finite or out-of-range double to
       an integer type is undefined behaviour (C11 6.3.1.4), which is
       precisely the float-cast-overflow the sanitizer jobs halt on -- and
       the harness is called from tests with deliberately awkward values.
     *
       Clamp rather than wrap: a negative delta larger than the bound would
       underflow size_t into an enormous allocation. */
    const double delta = Rf_asReal(r_cap_delta);
    size_t magnitude = 0;
    if (zu_int_size_from_real(delta < 0 ? -delta : delta, &magnitude) != 0) {
        return zu_int_result(ZU_ERR_INVALID_ARGUMENT, NULL, 0);
    }
    size_t cap = bound;
    if (delta < 0) {
        cap = (magnitude >= bound) ? 0 : bound - magnitude;
    } else if (delta > 0) {
        cap = bound + magnitude;
    }

    char    *vmax = vmaxget();
    uint8_t *dst  = (cap == 0) ? NULL : (uint8_t *) R_alloc(cap, 1);
    size_t   written = 0;

    st = zu_compress_one(&opts, src, n, dst, cap, &written);

    SEXP out = PROTECT(zu_int_result(st, dst, written));
    SEXP battr = PROTECT(Rf_ScalarReal((double) bound));
    Rf_setAttrib(out, Rf_install("bound"), battr);
    UNPROTECT(2);
    vmaxset(vmax);
    return out;
}

/* The struct_size forward-compatibility contract, which is otherwise
 * unreachable: registration happens once at init from vtables this build
 * compiled itself, so a vtable shorter than the current sizeof -- a
 * satellite codec built against an older header -- never occurs in the
 * suite. The defaulting rules are the risky part (a wrong answer here
 * silently mis-resolves "fast" for every third-party codec), so they are
 * pinned directly.
 *
 * Nothing is registered: these are pure reads of a local vtable, so the
 * registry stays read-only and the suite stays parallel-safe.
 *
 * Returns c(fast, best) for the requested shape:
 *   0  current header, both levels advertised   -> as advertised
 *   1  current header, both left 0              -> level_default
 *   2  older header that predates both fields   -> level_default
 */
SEXP zukomp_test_vtable_levels(SEXP r_case)
{
    zu_codec_vtable v;
    memset(&v, 0, sizeof(v));
    v.struct_size   = (uint32_t) sizeof(zu_codec_vtable);
    v.level_min     = 0;
    v.level_max     = 9;
    v.level_default = 6;

    switch (Rf_asInteger(r_case)) {
    case 0:
        v.level_fast = 1;
        v.level_best = 9;
        break;
    case 1:
        v.level_fast = 0;
        v.level_best = 0;
        break;
    case 2:
        /* A vtable that stops just short of the appended fields. The bytes
           are still set, so a reader that forgets the struct_size guard
           returns them and fails this case rather than passing by luck. */
        v.struct_size = (uint32_t) ZU_VTABLE_REQUIRED_SIZE;
        v.level_fast  = 12345;
        v.level_best  = 54321;
        break;
    default:
        return R_NilValue;
    }

    SEXP out = PROTECT(Rf_allocVector(INTSXP, 2));
    INTEGER(out)[0] = (int) zu_int_vtable_level_fast(&v);
    INTEGER(out)[1] = (int) zu_int_vtable_level_best(&v);
    UNPROTECT(1);
    return out;
}

/* zu_codec_get_info() into a caller struct that predates the appended
 * fields: the prefix must still be filled, and the appended fields must be
 * left exactly as the caller had them. Writing them would run past the end
 * of what an older consumer allocated.
 *
 * Returns c(status, level_min, level_default, level_fast, level_best) with
 * the last two read back out of the sentinel-filled struct. */
SEXP zukomp_test_info_short(SEXP r_codec, SEXP r_short)
{
    zu_codec codec = zu_codec_lookup(CHAR(STRING_ELT(r_codec, 0)));
    zu_codec_info info;
    memset(&info, 0, sizeof(info));
    info.level_fast = -999;          /* sentinels: untouched means untouched */
    info.level_best = -888;
    info.struct_size = (Rf_asLogical(r_short) == TRUE)
        ? (uint32_t) (offsetof(zu_codec_info, detectable) + sizeof(int))
        : (uint32_t) sizeof(zu_codec_info);

    zu_status st = zu_codec_get_info(codec, &info);

    SEXP out = PROTECT(Rf_allocVector(INTSXP, 5));
    INTEGER(out)[0] = (int) st;
    INTEGER(out)[1] = (int) info.level_min;
    INTEGER(out)[2] = (int) info.level_default;
    INTEGER(out)[3] = (int) info.level_fast;
    INTEGER(out)[4] = (int) info.level_best;
    UNPROTECT(1);
    return out;
}

/* Sinks currently allocated by the drive loop. The other leak tests watch
   R's Vcells, which cannot see a malloc'd buffer. */
SEXP zukomp_test_outbuf_live(void)
{
    return Rf_ScalarReal((double) zu_int_outbuf_live_count());
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
