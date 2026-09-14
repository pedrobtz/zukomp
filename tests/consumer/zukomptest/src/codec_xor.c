/* A third-party codec, registered into zukomp from outside it.
 *
 * XOR with 0x5A is not compression and is not encryption. It is chosen
 * precisely because it is trivial: what is being tested is the registry
 * contract, and any real algorithm here would put its own bugs between the
 * test and the thing under test.
 *
 * What this file must demonstrate:
 *   - a codec can live in another package entirely;
 *   - it registers at ZU_CODEC_VENDOR_BASE without zukomp knowing anything
 *     about it, and with no change to zukomp.h;
 *   - it becomes a first-class citizen of komp_codecs();
 *   - and, most importantly, zukomp's core limits still apply to it.
 */
#include <zukomp-r.h>

#define XOR_KEY 0x5A

/* Stateless, like zukomp's own identity codec: one non-NULL handle that is
   never dereferenced. */
static int xor5a_state;

static zu_status xor5a_new(void **st)
{
    if (st == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *st = &xor5a_state;
    return ZU_OK;
}

static zu_status xor5a_encoder_new(void **st, const zu_encoder_opts *o)
{ (void) o; return xor5a_new(st); }
static zu_status xor5a_decoder_new(void **st, const zu_decoder_opts *o)
{ (void) o; return xor5a_new(st); }

/* The transform is its own inverse, so one function serves both
   directions -- and the status discipline is the same one every codec owes
   the driver. */
static zu_status xor5a_process(void *st, zu_buffer *buf, zu_flush flush)
{
    (void) st;
    if (buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    size_t avail_in  = buf->src_size - buf->src_pos;
    size_t avail_out = buf->dst_size - buf->dst_pos;
    size_t n = (avail_in < avail_out) ? avail_in : avail_out;

    for (size_t i = 0; i < n; i++) {
        buf->dst[buf->dst_pos + i] =
            (uint8_t) (buf->src[buf->src_pos + i] ^ XOR_KEY);
    }
    buf->src_pos += n;
    buf->dst_pos += n;

    if (avail_in > n) {
        return ZU_NEED_OUTPUT;
    }
    if (flush == ZU_FINISH) {
        return ZU_STREAM_END;
    }
    return ZU_NEED_INPUT;
}

static zu_status xor5a_encoder_reset(void *st, const zu_encoder_opts *o)
{ (void) st; (void) o; return ZU_OK; }
static zu_status xor5a_decoder_reset(void *st, const zu_decoder_opts *o)
{ (void) st; (void) o; return ZU_OK; }
static void xor5a_free(void *st) { (void) st; }

static zu_status xor5a_bound(int32_t level, size_t n, size_t *out)
{
    (void) level;
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *out = n;
    return ZU_OK;
}

/* Note the codec id: ZU_CODEC_VENDOR_BASE, the range reserved for
   third-party registrations. zukomp has never heard of "xor5a" -- it is
   absent from the declared-name table -- so this also exercises the
   registry's path for codecs it does not know in advance. */
static const zu_codec_vtable xor5a_vtable = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_VENDOR_BASE,
    "xor5a",
    NULL,                 /* not an HTTP content-coding */
    "zukomptest",
    0, 0, 0,              /* no level axis */
    ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH,
    NULL, 0, 0, NULL,     /* nothing to detect: `auto` must never pick this */
    xor5a_encoder_new, xor5a_process, xor5a_encoder_reset, xor5a_free,
    xor5a_decoder_new, xor5a_process, xor5a_decoder_reset, xor5a_free,
    xor5a_bound
};

const zu_codec_vtable *zukomptest_vtable(void) { return &xor5a_vtable; }

/* Uses zukomp's one-shot C entry points through the API table -- the path a
   package like zuhttp takes, rather than the R API. */
SEXP zukomptest_roundtrip_via_c(SEXP bytes)
{
    const zukomp_api_v1 *api = zukomp_api();
    if (api == NULL) {
        Rf_error("zukomptest: could not resolve zukomp's API table");
    }

    const size_t n = (size_t) Rf_xlength(bytes);
    zu_encoder_opts eopts;
    memset(&eopts, 0, sizeof(eopts));
    eopts.struct_size = (uint32_t) sizeof(eopts);
    eopts.codec = (zu_codec) ZU_CODEC_VENDOR_BASE;
    eopts.level = ZU_LEVEL_DEFAULT;

    size_t cap = 0;
    if (api->compress_bound(eopts.codec, eopts.level, n, &cap) != ZU_OK) {
        Rf_error("zukomptest: compress_bound failed");
    }

    SEXP mid = PROTECT(Rf_allocVector(RAWSXP, (R_xlen_t) cap));
    size_t written = 0;
    zu_status st = api->compress_one(&eopts, (const uint8_t *) RAW(bytes), n,
                                     (uint8_t *) RAW(mid), cap, &written);
    if (st != ZU_OK) {
        UNPROTECT(1);
        Rf_error("zukomptest: compress_one: %s", api->status_string(st));
    }

    zu_decoder_opts dopts;
    memset(&dopts, 0, sizeof(dopts));
    dopts.struct_size = (uint32_t) sizeof(dopts);
    dopts.codec = eopts.codec;

    SEXP out = PROTECT(Rf_allocVector(RAWSXP, (R_xlen_t) n));
    size_t back = 0;
    st = api->decompress_one(&dopts, (const uint8_t *) RAW(mid), written,
                             (uint8_t *) RAW(out), n, &back);
    if (st != ZU_OK || back != n) {
        UNPROTECT(2);
        Rf_error("zukomptest: decompress_one: %s", api->status_string(st));
    }
    UNPROTECT(2);
    return out;
}

/* -- a declared codec, implemented by a satellite ---------------------------
 *
 * xor5a uses a vendor id, so registering it adds a row to komp_codecs(). The
 * interesting case for the R-side table cache is the opposite one: a
 * satellite implementing a codec zukomp already *declares*, which flips an
 * existing row from available = FALSE to TRUE without changing the row
 * count. A cache keyed on row count never rebuilds for it, so a table warmed
 * before this package loaded kept reporting the codec as not installed.
 *
 * snappy-raw is used purely as a declared identity to claim. This is not
 * Snappy -- it is the same trivial reversible transform as xor5a with a
 * different constant -- and the package is a test fixture that is never
 * installed for real use. What is under test is registry and cache
 * semantics, not codec compatibility. The declared name must match exactly,
 * which is itself one of the registration invariants. */
static zu_status snappy_stub_encoder_new(void **st, const zu_encoder_opts *o)
{ return xor5a_encoder_new(st, o); }
static zu_status snappy_stub_decoder_new(void **st, const zu_decoder_opts *o)
{ return xor5a_decoder_new(st, o); }

static const zu_codec_vtable snappy_stub_vtable = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_SNAPPY_RAW,
    "snappy-raw",         /* must equal the declared name, exactly */
    NULL,                 /* declared with no content-coding token */
    "zukomptest",
    0, 0, 0,
    ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH,
    NULL, 0, 0, NULL,     /* headerless: `auto` must never pick it */
    snappy_stub_encoder_new, xor5a_process, xor5a_encoder_reset, xor5a_free,
    snappy_stub_decoder_new, xor5a_process, xor5a_decoder_reset, xor5a_free,
    xor5a_bound
};

const zu_codec_vtable *zukomptest_declared_vtable(void)
{
    return &snappy_stub_vtable;
}

/* Attempts a deliberately invalid registration and returns the status, so
 * the registry's identity invariants can be tested from R. A rejected
 * registration mutates nothing, so these are safe to run in-process against
 * the shared, append-only registry.
 *
 * Each case is a way one satellite could poison codec discovery for the
 * whole session: registration is process-global and has no removal API. */
SEXP zukomptest_try_bad_registration(SEXP which)
{
    const zukomp_api_v1 *api = zukomp_api();
    if (api == NULL) {
        Rf_error("zukomptest: could not resolve zukomp's API table");
    }

    zu_codec_vtable v = snappy_stub_vtable;   /* a valid vtable to corrupt */

    switch (Rf_asInteger(which)) {
    case 0:  /* vendor id claiming a built-in's name */
        v.codec = (uint32_t) ZU_CODEC_VENDOR_BASE + 7;
        v.name  = "gzip";
        break;
    case 1:  /* declared id paired with the wrong name */
        v.codec = (uint32_t) ZU_CODEC_SNAPPY_RAW;
        v.name  = "not-snappy-raw";
        break;
    case 2:  /* unknown id below the vendor base */
        v.codec = 900;
        v.name  = "reserved-gap";
        break;
    case 3:  /* vendor id squatting a declared-but-absent name */
        v.codec = (uint32_t) ZU_CODEC_VENDOR_BASE + 8;
        v.name  = "zstd";
        break;
    case 4:  /* duplicate of a name already registered by this package */
        v.codec = (uint32_t) ZU_CODEC_VENDOR_BASE + 9;
        v.name  = "xor5a";
        break;
    case 5:  /* duplicate numeric id */
        v.codec = (uint32_t) ZU_CODEC_VENDOR_BASE;
        v.name  = "some-other-name";
        break;
    case 6:  /* vendor id claiming a declared content-coding token */
        v.codec = (uint32_t) ZU_CODEC_VENDOR_BASE + 10;
        v.name  = "fresh-name";
        v.content_encoding = "gzip";
        break;
    case 8:  /* declared id claiming ANOTHER declared codec's HTTP token */
        v.codec = (uint32_t) ZU_CODEC_SNAPPY_RAW;
        v.name  = "snappy-raw";
        v.content_encoding = "br";     /* brotli's, and brotli is declared */
        break;
    case 9:  /* declared id claiming an available codec's token */
        v.codec = (uint32_t) ZU_CODEC_SNAPPY_RAW;
        v.name  = "snappy-raw";
        v.content_encoding = "GZIP";   /* case-insensitively gzip's */
        break;
    case 7:  /* a vtable too short to carry the fields the core dereferences */
        v.codec = (uint32_t) ZU_CODEC_VENDOR_BASE + 11;
        v.name  = "stunted";
        v.struct_size = 8;
        break;
    default:
        return Rf_ScalarInteger(-1);
    }
    return Rf_ScalarInteger((int) api->register_codec(&v));
}
