/* The registered C-callable API table.
 *
 * LinkingTo hands a consumer our headers, not our object code, so this is
 * how the ABI actually reaches another package: one versioned table,
 * fetched with one R_GetCCallable lookup.
 *
 * This is the table consumption mode of design 15. The other one, the
 * static libzukomp.a installed under <pkg>/lib${R_ARCH}, deliberately
 * carries none of this: it is miniz's ZIP reader for consumers such as
 * zuxlsx, and no codec code. Shipping the *codecs* as an archive stays
 * rejected -- it would duplicate them into every consumer's shared object,
 * so a security fix would reach a consumer only when that consumer was
 * rebuilt, which is exactly the trade the table exists to avoid. */
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

#include "zukomp-r.h"
#include "zu_internal.h"

/* Static, so its address is stable for the life of the process and a
   consumer may cache the pointer -- which zukomp_api() does. */
static const zukomp_api_v1 zu_int_api_v1 = {
    (uint32_t) ZUKOMP_ABI_VERSION,
    (uint32_t) sizeof(zukomp_api_v1),

    zu_codec_lookup,
    zu_codec_from_content_encoding,
    zu_codec_available,
    zu_codec_get_info,
    zu_codec_list,
    zu_sniff,

    zu_encoder_new,
    zu_encoder_process,
    zu_encoder_reset,
    zu_encoder_free,

    zu_decoder_new,
    zu_decoder_process,
    zu_decoder_reset,
    zu_decoder_free,

    zu_compress_bound,
    zu_compress_one,
    zu_decompress_one,

    zu_status_string,
    zu_abi_version,
    zu_register_codec
};

/* Takes the ABI version the consumer was compiled against, and returns NULL
   rather than a best guess when it cannot satisfy it. A clean NULL at the
   consumer's call site beats a wild call through a table whose layout they
   disagree about. */
const zukomp_api_v1 *zukomp_get_api(uint32_t requested)
{
    if (requested != (uint32_t) ZUKOMP_ABI_VERSION) {
        return NULL;
    }
    return &zu_int_api_v1;
}

/* -- R-visible probes, for the test suite -------------------------------- */

SEXP zukomp_api_struct_size(void)
{
    return Rf_ScalarInteger((int) sizeof(zukomp_api_v1));
}

SEXP zukomp_get_api_r(SEXP requested)
{
    const zukomp_api_v1 *api = zukomp_get_api((uint32_t) Rf_asInteger(requested));
    if (api == NULL) {
        return R_NilValue;
    }
    /* Report something checkable rather than an opaque pointer: the table's
       own self-description, which is what a consumer validates. */
    SEXP out = PROTECT(Rf_allocVector(INTSXP, 2));
    INTEGER(out)[0] = (int) api->abi_version;
    INTEGER(out)[1] = (int) api->struct_size;
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, 2));
    SET_STRING_ELT(nms, 0, Rf_mkChar("abi_version"));
    SET_STRING_ELT(nms, 1, Rf_mkChar("struct_size"));
    Rf_setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}

/* Whether every function slot of the table is exactly the zu_* function of
 * the same name, as a named logical (#133).
 *
 * The thread contract in zukomp.h is a property of those functions: they
 * are pure C, call no R API and never unwind. ctest's ThreadSanitizer run
 * (ctest/test_threads.c) tests it on the zu_* functions themselves, with no
 * R in the process, which stands for the table only as long as the table
 * holds nothing else -- no wrapper that touches R, which would break the
 * contract on a worker thread while every test on R's main thread passed. */
SEXP zukomp_test_api_table_identity(void)
{
    const zukomp_api_v1 *api = zukomp_get_api((uint32_t) ZUKOMP_ABI_VERSION);
    const int n = 20;
    SEXP out = PROTECT(Rf_allocVector(LGLSXP, n));
    SEXP nms = PROTECT(Rf_allocVector(STRSXP, n));
    int i = 0;
#define ZU_INT_SLOT(slot, fn)                                              \
    do {                                                                   \
        LOGICAL(out)[i] = (api != NULL && api->slot == fn);                \
        SET_STRING_ELT(nms, i, Rf_mkChar(#slot));                          \
        i++;                                                               \
    } while (0)
    ZU_INT_SLOT(codec_lookup, zu_codec_lookup);
    ZU_INT_SLOT(codec_from_content_encoding, zu_codec_from_content_encoding);
    ZU_INT_SLOT(codec_available, zu_codec_available);
    ZU_INT_SLOT(codec_get_info, zu_codec_get_info);
    ZU_INT_SLOT(codec_list, zu_codec_list);
    ZU_INT_SLOT(sniff, zu_sniff);
    ZU_INT_SLOT(encoder_new, zu_encoder_new);
    ZU_INT_SLOT(encoder_process, zu_encoder_process);
    ZU_INT_SLOT(encoder_reset, zu_encoder_reset);
    ZU_INT_SLOT(encoder_free, zu_encoder_free);
    ZU_INT_SLOT(decoder_new, zu_decoder_new);
    ZU_INT_SLOT(decoder_process, zu_decoder_process);
    ZU_INT_SLOT(decoder_reset, zu_decoder_reset);
    ZU_INT_SLOT(decoder_free, zu_decoder_free);
    ZU_INT_SLOT(compress_bound, zu_compress_bound);
    ZU_INT_SLOT(compress_one, zu_compress_one);
    ZU_INT_SLOT(decompress_one, zu_decompress_one);
    ZU_INT_SLOT(status_string, zu_status_string);
    ZU_INT_SLOT(abi, zu_abi_version);
    ZU_INT_SLOT(register_codec, zu_register_codec);
#undef ZU_INT_SLOT
    Rf_setAttrib(out, R_NamesSymbol, nms);
    UNPROTECT(2);
    return out;
}
