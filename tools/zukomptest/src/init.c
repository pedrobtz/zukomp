#include <stdlib.h>

#include <R_ext/Visibility.h>

#include <zukomp-r.h>

const zu_codec_vtable *zukomptest_vtable(void);
const zu_codec_vtable *zukomptest_declared_vtable(void);
SEXP zukomptest_try_bad_registration(SEXP which);
SEXP zukomptest_roundtrip_via_c(SEXP bytes);
SEXP zukomptest_decodable_tokens(void);
SEXP zukomptest_codec_for_token(SEXP token);
SEXP zukomptest_decode_incremental(SEXP body, SEXP codec_name, SEXP chunk,
                                   SEXP max_output, SEXP max_ratio);

static const R_CallMethodDef call_methods[] = {
    {"zukomptest_roundtrip_via_c", (DL_FUNC) &zukomptest_roundtrip_via_c, 1},
    {"zukomptest_decodable_tokens", (DL_FUNC) &zukomptest_decodable_tokens, 0},
    {"zukomptest_codec_for_token",  (DL_FUNC) &zukomptest_codec_for_token,  1},
    {"zukomptest_decode_incremental", (DL_FUNC) &zukomptest_decode_incremental, 5},
    {"zukomptest_try_bad_registration",
                          (DL_FUNC) &zukomptest_try_bad_registration, 1},
    {NULL, NULL, 0}
};

void attribute_visible R_init_zukomptest(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, call_methods, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);

    /* Registration happens here because zukomp requires it before any
       encoder or decoder exists -- that is what keeps its registry
       read-only, and therefore thread-safe, for the rest of the session.
     *
     * This call working at all depends on the importFrom() in NAMESPACE
     * having already loaded zukomp's DLL. Without it, zukomp_api() returns
     * NULL here and the codec silently never appears. */
    const zukomp_api_v1 *api = zukomp_api();
    if (api == NULL) {
        Rf_error("zukomptest: zukomp's API table is unavailable; is there an "
                 "importFrom(zukomp, ...) in NAMESPACE?");
    }
    /* ZUKOMPTEST_DECLARED_ONLY exists for one test, and that test cannot be
       written without it. xor5a uses a vendor id, so registering it adds a
       komp_codecs() row -- which means it would invalidate a row-count-keyed
       cache all by itself and mask the very bug the test is for. Skipping it
       leaves only the declared-codec stub, whose registration changes no row
       count at all. */
    const char *declared_only = getenv("ZUKOMPTEST_DECLARED_ONLY");
    if (declared_only == NULL || declared_only[0] == '\0') {
        if (api->register_codec(zukomptest_vtable()) != ZU_OK) {
            Rf_error("zukomptest: failed to register the xor5a codec");
        }
    }
    /* A second registration, this time for a codec zukomp *declares* but does
       not implement. It changes an existing komp_codecs() row from
       unavailable to available without changing the row count, which is the
       case the R-side table cache has to notice. */
    if (api->register_codec(zukomptest_declared_vtable()) != ZU_OK) {
        Rf_error("zukomptest: failed to register the declared-codec stub");
    }
}
