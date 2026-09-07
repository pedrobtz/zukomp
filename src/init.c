#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Visibility.h>

/* No routines yet: the registry, stream driver and codecs arrive in later
   stages. Registering an empty table now fixes the entry-point shape, so
   adding the first .Call is a one-line change rather than a new file. */
static const R_CallMethodDef call_methods[] = {
    {NULL, NULL, 0}
};

void attribute_visible R_init_zukomp(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, call_methods, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);
}
