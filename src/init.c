#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>

#include "zu_internal.h"

SEXP zukomp_miniz_version(void);
SEXP zukomp_all_status_strings(void);
SEXP zukomp_abi_version(void);
SEXP zukomp_codec_table(void);
SEXP zukomp_codec_available(SEXP name);
SEXP zukomp_status_codes(void);
SEXP zukomp_test_stream(SEXP bytes, SEXP codec, SEXP encode, SEXP in_chunk,
                        SEXP out_chunk, SEXP max_output, SEXP max_ratio,
                        SEXP flush_every);
SEXP zukomp_test_grow(SEXP near_size_max);

static const R_CallMethodDef call_methods[] = {
    {"zukomp_miniz_version",      (DL_FUNC) &zukomp_miniz_version,      0},
    {"zukomp_all_status_strings", (DL_FUNC) &zukomp_all_status_strings, 0},
    {"zukomp_abi_version",        (DL_FUNC) &zukomp_abi_version,        0},
    {"zukomp_codec_table",        (DL_FUNC) &zukomp_codec_table,        0},
    {"zukomp_codec_available",    (DL_FUNC) &zukomp_codec_available,    1},
    {"zukomp_status_codes",       (DL_FUNC) &zukomp_status_codes,       0},
    {"zukomp_test_stream",        (DL_FUNC) &zukomp_test_stream,        8},
    {"zukomp_test_grow",          (DL_FUNC) &zukomp_test_grow,          1},
    {NULL, NULL, 0}
};

void attribute_visible R_init_zukomp(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, call_methods, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);

    /* The only point at which the registry is written. Doing it here, before
       any encoder or decoder can exist, is what makes the registry read-only
       for the rest of the session and the rest of zukomp thread-safe.
       Failure here is a programming error in this package, not anything a
       user can provoke, so refusing to load is the right response. */
    if (zu_int_register_builtin_codecs() != ZU_OK) {
        Rf_error("zukomp: failed to register built-in codecs");
    }
}
