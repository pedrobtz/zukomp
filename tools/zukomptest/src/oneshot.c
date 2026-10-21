/* zukomp's built-in zstd and lz4 codecs, driven through the table exactly
 * the way a columnar-format consumer does (zuarrow for Arrow IPC bodies,
 * qio for Parquet pages): one-shot compression at a level, and one-shot
 * decompression into a buffer sized from a length the container recorded,
 * with that length passed as ZU_DEC_EXPECTED_SIZE and max_output enforced.
 *
 * Everything goes through zukomp_api(); nothing here names a vendored
 * symbol. Output buffers are R_alloc'd, so an Rf_error() below cannot leak
 * them, and no stream handle outlives a call -- the one-shot functions own
 * theirs. */
#include <string.h>

#include <R_ext/Visibility.h>

#include <zukomp-r.h>

static const zukomp_api_v1 *oneshot_api(void)
{
    const zukomp_api_v1 *api = zukomp_api();
    if (api == NULL) {
        Rf_error("zukomptest: zukomp's API table is unavailable");
    }
    return api;
}

/* list(status = <zu_status name>, bytes = raw) -- the test asserts on the
   status name, which is what a C consumer branches on. */
static SEXP oneshot_result(const zukomp_api_v1 *api, zu_status st,
                           const uint8_t *p, size_t n)
{
    (void) api;
    static const char *names[] = {
        "ZU_OK", "ZU_NEED_INPUT", "ZU_NEED_OUTPUT", "ZU_STREAM_END",
        "ZU_ERR_INVALID_ARGUMENT", "ZU_ERR_UNSUPPORTED", "ZU_ERR_INVALID_DATA",
        "ZU_ERR_TRUNCATED", "ZU_ERR_CHECKSUM", "ZU_ERR_TRAILING",
        "ZU_ERR_MEMORY", "ZU_ERR_OUTPUT_LIMIT", "ZU_ERR_RATIO_LIMIT",
        "ZU_ERR_INTERNAL"
    };
    SEXP out = PROTECT(Rf_allocVector(VECSXP, 2));
    const char *nm = ((int) st >= 0 && (size_t) st < sizeof(names) / sizeof(names[0]))
                   ? names[st] : "unknown";
    SET_VECTOR_ELT(out, 0, Rf_mkString(nm));
    SEXP bytes = Rf_allocVector(RAWSXP, (R_xlen_t) n);
    SET_VECTOR_ELT(out, 1, bytes);
    if (n > 0) {
        memcpy(RAW(bytes), p, n);
    }
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, 2));
    SET_STRING_ELT(nms, 0, Rf_mkChar("status"));
    SET_STRING_ELT(nms, 1, Rf_mkChar("bytes"));
    Rf_setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}

static size_t oneshot_size(SEXP v)
{
    double d = Rf_asReal(v);
    if (!R_FINITE(d) || d < 0.0 || d > 9007199254740992.0) {
        Rf_error("zukomptest: a size must be a finite non-negative number");
    }
    return (size_t) d;
}

/* Compress with zu_compress_bound() sizing the buffer, as a consumer that
   writes a length-prefixed body does. NULL level = ZU_LEVEL_DEFAULT. */
SEXP zukomptest_compress_one(SEXP bytes, SEXP codec_name, SEXP level)
{
    const zukomp_api_v1 *api = oneshot_api();
    zu_codec codec = api->codec_lookup(CHAR(STRING_ELT(codec_name, 0)));

    zu_encoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec = codec;
    opts.level = Rf_isNull(level) ? ZU_LEVEL_DEFAULT : (int32_t) Rf_asInteger(level);

    size_t n = (size_t) Rf_xlength(bytes);
    size_t cap = 0;
    zu_status st = api->compress_bound(codec, opts.level, n, &cap);
    if (st != ZU_OK) {
        return oneshot_result(api, st, NULL, 0);
    }
    uint8_t *dst = (uint8_t *) R_alloc(cap > 0 ? cap : 1, 1);
    size_t written = 0;
    st = api->compress_one(&opts, RAW(bytes), n, dst, cap, &written);
    return oneshot_result(api, st, dst, written);
}

/* Decompress into a buffer of `cap` bytes. `expected` NULL means the
   container recorded no length; otherwise it is ZU_DEC_EXPECTED_SIZE.
   `old_struct` builds the options as a consumer compiled against zukomp
   0.1.0's header would: struct_size covering only the original fields. */
SEXP zukomptest_decompress_one(SEXP bytes, SEXP codec_name, SEXP cap_r,
                               SEXP max_output, SEXP expected, SEXP old_struct)
{
    const zukomp_api_v1 *api = oneshot_api();
    zu_decoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = Rf_asLogical(old_struct) == TRUE
                     ? (uint32_t) ZU_DECODER_OPTS_REQUIRED_SIZE
                     : (uint32_t) sizeof(opts);
    opts.codec      = api->codec_lookup(CHAR(STRING_ELT(codec_name, 0)));
    opts.max_output = (uint64_t) oneshot_size(max_output);
    opts.flags      = ZU_DEC_REJECT_TRAILING | ZU_DEC_CONCAT_MEMBERS;
    if (!Rf_isNull(expected)) {
        opts.flags |= ZU_DEC_EXPECTED_SIZE;
        opts.expected_size = (uint64_t) oneshot_size(expected);
    }

    size_t cap = oneshot_size(cap_r);
    uint8_t *dst = (uint8_t *) R_alloc(cap > 0 ? cap : 1, 1);
    size_t written = 0;
    zu_status st = api->decompress_one(&opts, RAW(bytes),
                                       (size_t) Rf_xlength(bytes),
                                       dst, cap, &written);
    return oneshot_result(api, st, dst, written);
}

/* The codec's capability row as the C side sees it, for the level checks. */
SEXP zukomptest_codec_levels(SEXP codec_name)
{
    const zukomp_api_v1 *api = oneshot_api();
    zu_codec codec = api->codec_lookup(CHAR(STRING_ELT(codec_name, 0)));
    zu_codec_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t) sizeof(info);
    if (api->codec_get_info(codec, &info) != ZU_OK) {
        return R_NilValue;
    }
    SEXP out = PROTECT(Rf_allocVector(INTSXP, 6));
    INTEGER(out)[0] = (int) info.codec;
    INTEGER(out)[1] = info.level_min;
    INTEGER(out)[2] = info.level_max;
    INTEGER(out)[3] = info.level_default;
    INTEGER(out)[4] = info.level_fast;
    INTEGER(out)[5] = info.level_best;
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, 6));
    const char *n[] = { "id", "min", "max", "default", "fast", "best" };
    for (int i = 0; i < 6; i++) {
        SET_STRING_ELT(nms, i, Rf_mkChar(n[i]));
    }
    Rf_setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}
