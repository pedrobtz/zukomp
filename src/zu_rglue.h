/* Internals that genuinely need R. Kept apart from zu_internal.h so the
   pure-C core stays compilable without R -- which is what lets fuzz/ build
   the codecs, driver and parsers directly against the ABI, with no R in the
   process at all. */
#ifndef ZU_RGLUE_H
#define ZU_RGLUE_H

#include <R.h>
#include <Rinternals.h>

#include "zu_internal.h"

/* Packs a native status and the bytes produced so far into the pair every
   R-visible entry point returns, so R -- never C -- decides what is a
   condition (design 13 rule 1). Defined in src/zukomp_test.c. */
SEXP zu_int_result(zu_status status, const uint8_t *bytes, size_t n);

/* Narrowing R numerics for C. Each returns non-zero when the value cannot
   be represented, rather than producing a silently wrong one. Defined in
   src/zukomp_r.c. */
int zu_int_level_from_sexp(SEXP r_level, int32_t *out);
int zu_int_u64_from_real(double v, uint64_t *out);
int zu_int_u32_from_int(int v, uint32_t *out);

#endif /* ZU_RGLUE_H */
