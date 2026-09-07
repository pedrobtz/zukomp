/* R-visible entry points. Everything here is glue: it converts between SEXP
   and the pure-C ABI in zukomp.h and raises no condition of its own beyond
   argument validation. Entry points are prefixed zukomp_ per design 14. */
#include <R.h>
#include <Rinternals.h>

#include "zu_internal.h"

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

/* -- registry projection ------------------------------------------------- */

static SEXP zu_int_str_or_na(const char *s)
{
    return (s == NULL) ? NA_STRING : Rf_mkChar(s);
}

/* Resolves a length-1 character codec name to its identity, or ZU_CODEC_NONE.
   Raising the condition is R's job, so this only reports. */
static zu_codec zu_int_codec_from_sexp(SEXP name)
{
    if (TYPEOF(name) != STRSXP || Rf_xlength(name) != 1 ||
        STRING_ELT(name, 0) == NA_STRING) {
        return ZU_CODEC_NONE;
    }
    return zu_codec_lookup(CHAR(STRING_ELT(name, 0)));
}

SEXP zukomp_codec_available(SEXP name)
{
    zu_codec codec = zu_int_codec_from_sexp(name);
    if (codec == ZU_CODEC_NONE) {
        return Rf_ScalarLogical(NA_LOGICAL);   /* name unknown to this build */
    }
    return Rf_ScalarLogical(zu_codec_available(codec) ? TRUE : FALSE);
}

/* Every codec this build knows the name of, followed by any third-party
   registrations, as parallel vectors for komp_codecs() to make a data frame
   from. Building the frame in R keeps this function free of attribute
   fiddling; building the *columns* in C keeps the registry the single
   source of truth. */
SEXP zukomp_codec_table(void)
{
    const size_t n_declared = zu_int_declared_count();
    const size_t n_reg      = zu_int_registry_count();

    /* Third-party codecs are absent from the declared table, so count them
       separately rather than assuming registered implies declared.
     *
     * NOTE: n_extra is necessarily 0 until a satellite package registers a
     * codec, so this branch and the row-selection loop below are not
     * exercised by the current suite. Stage 12's consumer package is what
     * first proves them; registration is deliberately C-only and
     * init-time-only, so there is no way to reach it from R without
     * breaking the read-only-after-init invariant that lets the test suite
     * run in parallel. Treat this path as unverified until then. */
    size_t n_extra = 0;
    for (size_t i = 0; i < n_reg; i++) {
        const zu_codec_vtable *v = zu_int_registry_at(i);
        if (zu_int_declared_for((zu_codec) v->codec) == NULL) {
            n_extra++;
        }
    }

    const R_xlen_t n = (R_xlen_t) (n_declared + n_extra);
    const int n_col = 10;

    SEXP out = PROTECT(Rf_allocVector(VECSXP, n_col));

    SEXP id       = Rf_allocVector(STRSXP, n); SET_VECTOR_ELT(out, 0, id);
    SEXP avail    = Rf_allocVector(LGLSXP, n); SET_VECTOR_ELT(out, 1, avail);
    SEXP can_enc  = Rf_allocVector(LGLSXP, n); SET_VECTOR_ELT(out, 2, can_enc);
    SEXP can_dec  = Rf_allocVector(LGLSXP, n); SET_VECTOR_ELT(out, 3, can_dec);
    SEXP lvl_min  = Rf_allocVector(INTSXP, n); SET_VECTOR_ELT(out, 4, lvl_min);
    SEXP lvl_max  = Rf_allocVector(INTSXP, n); SET_VECTOR_ELT(out, 5, lvl_max);
    SEXP lvl_def  = Rf_allocVector(INTSXP, n); SET_VECTOR_ELT(out, 6, lvl_def);
    SEXP detect   = Rf_allocVector(LGLSXP, n); SET_VECTOR_ELT(out, 7, detect);
    SEXP ce       = Rf_allocVector(STRSXP, n); SET_VECTOR_ELT(out, 8, ce);
    SEXP source   = Rf_allocVector(STRSXP, n); SET_VECTOR_ELT(out, 9, source);

    for (R_xlen_t row = 0; row < n; row++) {
        const zu_int_codec_decl *decl = NULL;
        const zu_codec_vtable   *v    = NULL;

        if ((size_t) row < n_declared) {
            decl = zu_int_declared_at((size_t) row);
            v    = zu_int_registry_lookup(decl->codec);
        } else {
            /* walk the registry again for the row-th undeclared codec */
            size_t want = (size_t) row - n_declared, seen = 0;
            for (size_t i = 0; i < n_reg; i++) {
                const zu_codec_vtable *cand = zu_int_registry_at(i);
                if (zu_int_declared_for((zu_codec) cand->codec) != NULL) {
                    continue;
                }
                if (seen++ == want) { v = cand; break; }
            }
        }

        const char *name = (v != NULL) ? v->name : decl->name;
        SET_STRING_ELT(id, row, Rf_mkChar(name));
        LOGICAL(avail)[row] = (v != NULL) ? TRUE : FALSE;

        if (v == NULL) {
            /* Name known, implementation absent: everything capability-shaped
               is genuinely unknown, so NA rather than a misleading FALSE. */
            LOGICAL(can_enc)[row] = NA_LOGICAL;
            LOGICAL(can_dec)[row] = NA_LOGICAL;
            LOGICAL(detect)[row]  = NA_LOGICAL;
            INTEGER(lvl_min)[row] = NA_INTEGER;
            INTEGER(lvl_max)[row] = NA_INTEGER;
            INTEGER(lvl_def)[row] = NA_INTEGER;
            SET_STRING_ELT(ce, row, zu_int_str_or_na(decl->content_encoding));
            SET_STRING_ELT(source, row, NA_STRING);
            continue;
        }

        LOGICAL(can_enc)[row] = (v->flags & ZU_CAN_ENCODE) ? TRUE : FALSE;
        LOGICAL(can_dec)[row] = (v->flags & ZU_CAN_DECODE) ? TRUE : FALSE;
        LOGICAL(detect)[row]  = (v->magic != NULL || v->sniff != NULL)
                                ? TRUE : FALSE;

        /* All three zero is the header's encoding of "this codec has no
           level axis" (identity, and later snappy). Report NA rather than a
           0..0 range that invites callers to pass level = 0. */
        int has_levels = !(v->level_min == 0 && v->level_max == 0 &&
                           v->level_default == 0);
        INTEGER(lvl_min)[row] = has_levels ? v->level_min : NA_INTEGER;
        INTEGER(lvl_max)[row] = has_levels ? v->level_max : NA_INTEGER;
        INTEGER(lvl_def)[row] = has_levels ? v->level_default : NA_INTEGER;

        SET_STRING_ELT(ce, row, zu_int_str_or_na(v->content_encoding));
        SET_STRING_ELT(source, row, zu_int_str_or_na(v->source));
    }

    UNPROTECT(1);
    return out;
}
