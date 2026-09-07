/* The zuhttp integration contract (design 16), exercised from a real
 * consumer without an HTTP client.
 *
 * Stage 15 is written against zuhttp, which does not exist yet. What can be
 * proved without it is the half that actually concerns zukomp: that an HTTP
 * client can derive its Accept-Encoding from the registry, resolve
 * content-coding tokens, and decode a response body incrementally through
 * the C ABI, never materialising the whole thing.
 *
 * Everything here goes through zukomp_api(), never through zukomp's R
 * functions, because that is the path zuhttp will take.
 */
#include <R_ext/Visibility.h>

#include <zukomp-r.h>

/* Contract point 1: Accept-Encoding is derived, not hardcoded.
 *
 * Walks zu_codec_list() and reports the content-coding token of every codec
 * that can decode. Installing a satellite codec makes its token appear here
 * with no change to this function -- which is the whole reason zuhttp is
 * told to build the header this way. */
SEXP zukomptest_decodable_tokens(void)
{
    const zukomp_api_v1 *api = zukomp_api();
    if (api == NULL) {
        Rf_error("zukomptest: zukomp's API table is unavailable");
    }

    zu_codec ids[64];
    size_t n = 0;
    if (api->codec_list(ids, sizeof(ids) / sizeof(ids[0]), &n) != ZU_OK) {
        Rf_error("zukomptest: codec_list failed");
    }

    SEXP out = PROTECT(Rf_allocVector(STRSXP, (R_xlen_t) n));
    R_xlen_t k = 0;
    for (size_t i = 0; i < n; i++) {
        zu_codec_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t) sizeof(info);
        if (api->codec_get_info(ids[i], &info) != ZU_OK) {
            continue;
        }
        /* Only codecs we can actually decode, and only those that are an
           HTTP content-coding at all: deflate-raw is neither. */
        if (!(info.flags & ZU_CAN_DECODE) || info.content_encoding == NULL) {
            continue;
        }
        SET_STRING_ELT(out, k++, Rf_mkChar(info.content_encoding));
    }
    SEXP trimmed = PROTECT(Rf_allocVector(STRSXP, k));
    for (R_xlen_t i = 0; i < k; i++) {
        SET_STRING_ELT(trimmed, i, STRING_ELT(out, i));
    }
    UNPROTECT(2);
    return trimmed;
}

/* Resolves an HTTP content-coding token to a codec name.
 *
 * Contract point 3 lives on the other side of this: `deflate` resolves to
 * zlib here, and the retry-as-raw policy for servers that actually send
 * headerless DEFLATE is zuhttp's, not zukomp's. */
SEXP zukomptest_codec_for_token(SEXP token)
{
    const zukomp_api_v1 *api = zukomp_api();
    if (api == NULL) {
        Rf_error("zukomptest: zukomp's API table is unavailable");
    }
    zu_codec codec = api->codec_from_content_encoding(CHAR(STRING_ELT(token, 0)));
    if (codec == ZU_CODEC_NONE) {
        return Rf_ScalarString(NA_STRING);
    }
    zu_codec_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t) sizeof(info);
    if (api->codec_get_info(codec, &info) != ZU_OK) {
        return Rf_ScalarString(NA_STRING);
    }
    return Rf_ScalarString(Rf_mkChar(info.name));
}

/* Contract points 2 and 4: decode a response body incrementally.
 *
 * This is the shape of criterion 11. The body arrives in `chunk`-sized
 * pieces and is decoded into a fixed sink of the same size, which is
 * reused; nothing accumulates. The function returns only the decoded
 * length and a checksum, so a test can assert correctness without the
 * decoded body ever existing in one piece -- which is exactly the property
 * an HTTP client needs and the one that a whole-buffer API cannot offer.
 *
 * Limits come from the caller and are enforced by zukomp's driver, not
 * here: max_decompressed_bytes and max_decompression_ratio map straight
 * onto zu_decoder_opts. */
SEXP zukomptest_decode_incremental(SEXP body, SEXP codec_name, SEXP r_chunk,
                                   SEXP r_max_output, SEXP r_max_ratio)
{
    const zukomp_api_v1 *api = zukomp_api();
    if (api == NULL) {
        Rf_error("zukomptest: zukomp's API table is unavailable");
    }

    const uint8_t *src = (const uint8_t *) RAW(body);
    const size_t   n   = (size_t) Rf_xlength(body);
    const size_t   chunk = (size_t) Rf_asInteger(r_chunk);

    zu_codec codec = api->codec_lookup(CHAR(STRING_ELT(codec_name, 0)));
    if (codec == ZU_CODEC_NONE) {
        Rf_error("zukomptest: unknown codec");
    }
    /* A zero-size sink clamps every `take` to 0, so `fed` never advances,
       `last` is never reached, and the loop spins forever with no way out.
       zukomp's own driver rejects in_chunk == 0 for exactly this reason;
       a consumer has to do the same at its own boundary. */
    if (chunk == 0 || Rf_asInteger(r_chunk) == NA_INTEGER) {
        Rf_error("zukomptest: chunk must be a positive number of bytes");
    }

    zu_decoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec      = codec;
    opts.max_output = (uint64_t) Rf_asReal(r_max_output);
    opts.max_ratio  = (uint32_t) Rf_asInteger(r_max_ratio);
    opts.flags      = ZU_DEC_CONCAT_MEMBERS | ZU_DEC_REJECT_TRAILING;

    zu_decoder *dec = NULL;
    zu_status st = api->decoder_new(&dec, &opts);
    if (st != ZU_OK) {
        Rf_error("zukomptest: decoder_new: %s", api->status_string(st));
    }

    /* The sink. One chunk, reused for the whole response. */
    SEXP sink = PROTECT(Rf_allocVector(RAWSXP, (R_xlen_t) chunk));
    uint8_t *out = (uint8_t *) RAW(sink);

    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));

    uint64_t total = 0;
    unsigned spins = 0;
    uint32_t sum = 0;          /* trivial rolling checksum over the output */
    size_t fed = 0;

    for (;;) {
        if (buf.src_pos == buf.src_size && fed < n) {
            size_t take = n - fed;
            if (take > chunk) { take = chunk; }
            buf.src = src + fed;
            buf.src_size = take;
            buf.src_pos = 0;
            fed += take;
        }
        int last = (fed >= n) && (buf.src_pos == buf.src_size);

        buf.dst = out;
        buf.dst_size = chunk;
        buf.dst_pos = 0;

        st = api->decoder_process(dec, &buf, last ? ZU_FINISH : ZU_RUN);

        for (size_t i = 0; i < buf.dst_pos; i++) {
            sum = sum * 31u + out[i];      /* the sink is consumed, not kept */
        }
        total += (uint64_t) buf.dst_pos;

        if (st == ZU_STREAM_END) { break; }
        if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) { break; }
        if (buf.dst_pos == 0 && last && st == ZU_NEED_INPUT) { break; }

        /* Decoding a large body must stay interruptible. Safe here because
           the only heap state is the decoder, and R owns the sink -- but
           note this is precisely the hazard design 13 rule 3 covers, so a
           real client holding more state would need the external-pointer
           treatment zukomp's own driver uses. */
        if ((++spins % 64u) == 0u) {
            R_CheckUserInterrupt();
        }
    }

    const char *status = api->status_string(st);
    int failed = (st != ZU_STREAM_END);
    api->decoder_free(dec);
    UNPROTECT(1);

    SEXP res = PROTECT(Rf_allocVector(VECSXP, 4));
    SET_VECTOR_ELT(res, 0, Rf_ScalarLogical(!failed));
    SET_VECTOR_ELT(res, 1, Rf_ScalarReal((double) total));
    SET_VECTOR_ELT(res, 2, Rf_ScalarReal((double) sum));
    SET_VECTOR_ELT(res, 3, Rf_mkString(status));
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, 4));
    SET_STRING_ELT(nms, 0, Rf_mkChar("ok"));
    SET_STRING_ELT(nms, 1, Rf_mkChar("bytes"));
    SET_STRING_ELT(nms, 2, Rf_mkChar("checksum"));
    SET_STRING_ELT(nms, 3, Rf_mkChar("status"));
    Rf_setAttrib(res, R_NamesSymbol, nms);
    UNPROTECT(2);
    return res;
}
