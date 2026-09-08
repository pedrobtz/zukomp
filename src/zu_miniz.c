/* Temporary Stage 1 scaffolding: proves miniz is compiled in and reachable,
   and pins the version so a silent upstream swap is a test failure rather
   than a surprise. Deleted once komp_info() reports vendored versions
   properly (Stage 9). */
#include <R.h>
#include <Rinternals.h>

#include "miniz.h"

SEXP zukomp_miniz_version(void)
{
    return Rf_mkString(MZ_VERSION);
}
