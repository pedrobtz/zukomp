/* R-visible entry points. Everything here is glue: it converts between SEXP
   and the pure-C ABI in zukomp.h and raises no condition of its own beyond
   argument validation. Entry points are prefixed zukomp_ per design 14. */
#include <R.h>
#include <Rinternals.h>

#include "zukomp.h"

/* Walks 0..ZU_ERR_INTERNAL and returns zu_status_string() for each, so the
   test suite can assert that every enumerator has a real description
   without hardcoding the list on the R side. */
SEXP zukomp_all_status_strings(void)
{
    const int n = (int) ZU_ERR_INTERNAL + 1;
    SEXP out = PROTECT(Rf_allocVector(STRSXP, n));
    for (int i = 0; i < n; i++) {
        SET_STRING_ELT(out, i, Rf_mkChar(zu_status_string((zu_status) i)));
    }
    UNPROTECT(1);
    return out;
}

SEXP zukomp_abi_version(void)
{
    return Rf_ScalarInteger((int) zu_abi_version());
}
