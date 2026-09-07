/* The registered C-callable API table.
 *
 * LinkingTo hands a consumer our headers, not our object code, so this is
 * how the ABI actually reaches another package: one versioned table,
 * fetched with one R_GetCCallable lookup.
 *
 * Rejected alternative (design 15): shipping inst/lib/libzukomp.a. It
 * duplicates the codec code into every consumer's shared object, which
 * defeats the point of having one place to apply a security update, and it
 * adds PIC and library-path handling on three platforms. */
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
    SEXP nms = Rf_allocVector(STRSXP, 2);
    Rf_setAttrib(out, R_NamesSymbol, nms);
    SET_STRING_ELT(nms, 0, Rf_mkChar("abi_version"));
    SET_STRING_ELT(nms, 1, Rf_mkChar("struct_size"));
    UNPROTECT(1);
    return out;
}
