/* Consumes zukomp exactly as zuxlsx will: miniz's own ZIP reader, off the
 * LinkingTo include path, linked statically out of inst/lib/libzukomp.a.
 * Nothing here includes zukomp.h, calls zu_*, or touches zukomp's namespace.
 *
 * The three entry points fail in different ways, on purpose:
 *
 *   C_miniz_version   a real call, not a macro, so the archive has to supply
 *                     a definition for it. On Linux and Windows a dropped
 *                     PKG_LIBS therefore fails at link time. On macOS it does
 *                     not -- R links with -undefined dynamic_lookup and the
 *                     loader quietly satisfies mz_* from whatever miniz is
 *                     already in the process -- which is why
 *                     tools/check-linking.sh checks nm -u as well.
 *   C_zip_members     reads the central directory. m_time is the field that
 *                     matters: src/Makevars compiles this archive with
 *                     -UMINIZ_NO_TIME so a consumer compiling miniz.h at its
 *                     defaults reads the field it thinks it is reading. Had
 *                     the archive kept the trim, mz_zip_archive_file_stat
 *                     would end in m_padding instead, the two would still be
 *                     the same size, and nothing miniz reports would say so.
 *   C_zip_extract     drives the extraction iterator at a caller-chosen chunk
 *                     size. A pull-style reader such as xlsxio needs exactly
 *                     this, and it is the reason a downstream package links
 *                     this archive rather than going through the codec
 *                     registry. A wrongly built archive shows up here, at
 *                     run time.
 */

#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

#include <miniz.h>

#include <stddef.h>
#include <string.h>

/* Ceilings on anything the archive's central directory claims. Both numbers
 * are read out of the input, and zukomp's own rule is that a size claimed by
 * the input never sizes a buffer unbounded -- the fixture has no licence to
 * be sloppier about that than the package it is testing. 16 MiB is the cap
 * the fuzz targets use. */
#define ZL_MAX_OUTPUT  (16u * 1024u * 1024u)
#define ZL_MAX_MEMBERS 1024u

/* Every helper below opens, works and closes without allocating R memory in
 * between. That is deliberate: Rf_error() and Rf_allocVector()'s own failure
 * path both longjmp, and either one firing while an mz_zip_archive is open
 * would leak its internal state and, on the reader-from-file path, an open
 * FILE*. So each entry point runs a sizing pass, allocates, then runs a
 * filling pass, and raises nothing while a handle is live. */

static const char *zl_last_error(mz_zip_archive *zip)
{
    return mz_zip_get_error_string(mz_zip_get_last_error(zip));
}

/* Sizing pass: how many members, and does the archive open at all. */
static int zl_count(const char *path, mz_uint *out_n, const char **err)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        *err = zl_last_error(&zip);
        mz_zip_reader_end(&zip);
        return 0;
    }
    *out_n = mz_zip_reader_get_num_files(&zip);
    mz_zip_reader_end(&zip);

    if (*out_n > ZL_MAX_MEMBERS) {
        *err = "archive claims more members than this fixture will read";
        return 0;
    }
    return 1;
}

SEXP C_zip_members(SEXP path_)
{
    const char *path = CHAR(STRING_ELT(path_, 0));
    const char *err = NULL;
    mz_zip_archive zip;
    mz_uint n = 0, i;
    SEXP name, usize, csize, mtime, ans, nms;

    if (!zl_count(path, &n, &err)) {
        Rf_error("zukomplink: %s", err);
    }

    name  = PROTECT(Rf_allocVector(STRSXP,  (R_xlen_t) n));
    usize = PROTECT(Rf_allocVector(REALSXP, (R_xlen_t) n));
    csize = PROTECT(Rf_allocVector(REALSXP, (R_xlen_t) n));
    mtime = PROTECT(Rf_allocVector(REALSXP, (R_xlen_t) n));

    /* Filling pass. No R allocation from here until the reader is closed. */
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        err = zl_last_error(&zip);
        mz_zip_reader_end(&zip);
        UNPROTECT(4);
        Rf_error("zukomplink: %s", err);
    }

    for (i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            err = zl_last_error(&zip);
            break;
        }
        SET_STRING_ELT(name, (R_xlen_t) i, Rf_mkChar(st.m_filename));
        REAL(usize)[i] = (double) st.m_uncomp_size;
        REAL(csize)[i] = (double) st.m_comp_size;
        /* m_time, not m_padding: see the header comment. */
        REAL(mtime)[i] = (double) st.m_time;
    }
    mz_zip_reader_end(&zip);

    if (err != NULL) {
        UNPROTECT(4);
        Rf_error("zukomplink: %s", err);
    }

    ans = PROTECT(Rf_allocVector(VECSXP, 4));
    SET_VECTOR_ELT(ans, 0, name);
    SET_VECTOR_ELT(ans, 1, usize);
    SET_VECTOR_ELT(ans, 2, csize);
    SET_VECTOR_ELT(ans, 3, mtime);
    nms = PROTECT(Rf_allocVector(STRSXP, 4));
    SET_STRING_ELT(nms, 0, Rf_mkChar("name"));
    SET_STRING_ELT(nms, 1, Rf_mkChar("uncomp_size"));
    SET_STRING_ELT(nms, 2, Rf_mkChar("comp_size"));
    SET_STRING_ELT(nms, 3, Rf_mkChar("mtime"));
    Rf_setAttrib(ans, R_NamesSymbol, nms);

    UNPROTECT(6);
    return ans;
}

/* Sizing pass for one member: locate it and read its declared size. */
static int zl_locate(const char *path, const char *name, mz_uint *out_index,
                     mz_uint64 *out_size, const char **err)
{
    mz_zip_archive zip;
    mz_zip_archive_file_stat st;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        *err = zl_last_error(&zip);
        mz_zip_reader_end(&zip);
        return 0;
    }
    if (!mz_zip_reader_locate_file_v2(&zip, name, NULL,
                                      MZ_ZIP_FLAG_CASE_SENSITIVE, out_index)) {
        *err = "no such member in the archive";
        mz_zip_reader_end(&zip);
        return 0;
    }
    if (!mz_zip_reader_file_stat(&zip, *out_index, &st)) {
        *err = zl_last_error(&zip);
        mz_zip_reader_end(&zip);
        return 0;
    }
    mz_zip_reader_end(&zip);

    if (st.m_uncomp_size > (mz_uint64) ZL_MAX_OUTPUT) {
        *err = "member claims to decompress past this fixture's output cap";
        return 0;
    }
    *out_size = st.m_uncomp_size;
    return 1;
}

SEXP C_zip_extract(SEXP path_, SEXP name_, SEXP chunk_)
{
    const char *path = CHAR(STRING_ELT(path_, 0));
    const char *name = CHAR(STRING_ELT(name_, 0));
    int chunk_arg = Rf_asInteger(chunk_);
    const char *err = NULL;
    mz_zip_archive zip;
    mz_zip_reader_extract_iter_state *iter;
    mz_uint index = 0;
    mz_uint64 want = 0;
    size_t chunk, total = 0;
    SEXP ans;

    if (!zl_locate(path, name, &index, &want, &err)) {
        Rf_error("zukomplink: %s", err);
    }

    /* 0 means "whatever is left", which is the unchunked path. Anything else
     * is taken literally, down to 1 byte per call: a reader that only works
     * when it is handed the whole member at once is not a streaming reader,
     * and that is the property this fixture exists to hold. */
    chunk = (chunk_arg > 0) ? (size_t) chunk_arg : (size_t) want;
    if (chunk == 0) chunk = 1;

    ans = PROTECT(Rf_allocVector(RAWSXP, (R_xlen_t) want));

    /* No R allocation from here until the reader is closed. */
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        err = zl_last_error(&zip);
        mz_zip_reader_end(&zip);
        UNPROTECT(1);
        Rf_error("zukomplink: %s", err);
    }

    iter = mz_zip_reader_extract_iter_new(&zip, index, 0);
    if (iter == NULL) {
        err = zl_last_error(&zip);
        mz_zip_reader_end(&zip);
        UNPROTECT(1);
        Rf_error("zukomplink: %s", err);
    }

    for (;;) {
        size_t room = (size_t) want - total;
        size_t ask = (room < chunk) ? room : chunk;
        size_t got;
        if (ask == 0) break;
        got = mz_zip_reader_extract_iter_read(iter, RAW(ans) + total, ask);
        if (got == 0) break;
        total += got;
    }

    /* iter_free is what runs the CRC-32 check and reports a mismatch, so its
     * return value is the decode's verdict and must not be discarded. */
    if (!mz_zip_reader_extract_iter_free(iter)) {
        err = "member failed its CRC-32 check";
    }
    mz_zip_reader_end(&zip);

    if (err == NULL && total != (size_t) want) {
        err = "member decompressed to a different size than it declared";
    }
    if (err != NULL) {
        UNPROTECT(1);
        Rf_error("zukomplink: %s", err);
    }

    UNPROTECT(1);
    return ans;
}

SEXP C_miniz_version(void)
{
    /* A call, not MZ_VERSION: the macro would be satisfied by the header
     * alone and would still compile with PKG_LIBS empty. */
    return Rf_mkString(mz_version());
}

static const R_CallMethodDef call_methods[] = {
    {"C_zip_members",   (DL_FUNC) &C_zip_members,   1},
    {"C_zip_extract",   (DL_FUNC) &C_zip_extract,   3},
    {"C_miniz_version", (DL_FUNC) &C_miniz_version, 0},
    {NULL, NULL, 0}
};

void R_init_zukomplink(DllInfo *dll)
{
    R_registerRoutines(dll, NULL, call_methods, NULL, NULL);
    R_useDynamicSymbols(dll, FALSE);
    R_forceSymbols(dll, TRUE);
}
