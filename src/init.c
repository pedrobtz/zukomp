#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>

SEXP zukomp_miniz_version(void);

static const R_CallMethodDef call_methods[] = {
    {"zukomp_miniz_version", (DL_FUNC) &zukomp_miniz_version, 0},
    {NULL, NULL, 0}
};

void attribute_visible R_init_zukomp(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, call_methods, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);
}
