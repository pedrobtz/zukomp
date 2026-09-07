#include <R_ext/Visibility.h>

#include <zukomp-r.h>

const zu_codec_vtable *zukomptest_vtable(void);
SEXP zukomptest_roundtrip_via_c(SEXP bytes);

static const R_CallMethodDef call_methods[] = {
    {"zukomptest_roundtrip_via_c", (DL_FUNC) &zukomptest_roundtrip_via_c, 1},
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
    if (api->register_codec(zukomptest_vtable()) != ZU_OK) {
        Rf_error("zukomptest: failed to register the xor5a codec");
    }
}
