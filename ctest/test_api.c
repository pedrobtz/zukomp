/* Discovery and registration: the C ABI's lookup functions, and the rules
 * zu_register_codec() enforces on a satellite's vtable.
 *
 * The registry suite registers codecs, and the registry has no removal API,
 * so it runs last (test_main.c). */
#include "zkt.h"

static void zkt_status_strings(void)
{
    /* Every status has its own string, and none is the fallback. */
    const char *seen[ZU_ERR_INTERNAL + 1];
    for (int s = ZU_OK; s <= ZU_ERR_INTERNAL; s++) {
        seen[s] = zu_status_string((zu_status) s);
        ZKT_CHECK(seen[s] != NULL);
        ZKT_CHECK(strcmp(seen[s], "unrecognised status") != 0);
        for (int t = ZU_OK; t < s; t++) {
            ZKT_CHECK(strcmp(seen[s], seen[t]) != 0);
        }
    }
    ZKT_CHECK(strcmp(zu_status_string(ZU_OK), "ok") == 0);
    ZKT_CHECK(strcmp(zu_status_string(ZU_ERR_MEMORY), "out of memory") == 0);
    ZKT_CHECK(strcmp(zu_status_string(ZU_ERR_CHECKSUM), "checksum mismatch") == 0);
    ZKT_CHECK(strcmp(zu_status_string((zu_status) 99), "unrecognised status") == 0);
    ZKT_CHECK(zu_abi_version() == ZUKOMP_ABI_VERSION);
    ZKT_CHECK(zu_abi_version() == 1u);
}

static void zkt_lookup(void)
{
    static const struct { const char *name; zu_codec codec; int available; } t[] = {
        { "identity", ZU_CODEC_IDENTITY, 1 },
        { "deflate-raw", ZU_CODEC_DEFLATE_RAW, 1 },
        { "zlib", ZU_CODEC_ZLIB, 1 },
        { "gzip", ZU_CODEC_GZIP, 1 },
        { "brotli", ZU_CODEC_BROTLI, 0 },
        { "zstd", ZU_CODEC_ZSTD, 1 },
        { "lz4-frame", ZU_CODEC_LZ4_FRAME, 1 },
        { "lz4-block", ZU_CODEC_LZ4_BLOCK, 1 },
        { "snappy-frame", ZU_CODEC_SNAPPY_FRAME, 0 },
        { "snappy-raw", ZU_CODEC_SNAPPY_RAW, 0 }
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        ZKT_CHECK(zu_codec_lookup(t[i].name) == t[i].codec);
        ZKT_CHECK(zu_codec_available(t[i].codec) == t[i].available);
        const zu_int_codec_decl *d = zu_int_declared_for(t[i].codec);
        ZKT_CHECK(d != NULL && strcmp(d->name, t[i].name) == 0);
        ZKT_CHECK(zu_int_declared_at(i) != NULL &&
                  zu_int_declared_at(i)->codec == t[i].codec);
    }
    ZKT_CHECK(zu_int_declared_count() == 10);
    ZKT_CHECK(zu_int_declared_at(10) == NULL);
    ZKT_CHECK(zu_int_declared_for((zu_codec) 999) == NULL);
    ZKT_CHECK(zu_codec_lookup(NULL) == ZU_CODEC_NONE);
    ZKT_CHECK(zu_codec_lookup("deflate") == ZU_CODEC_NONE);   /* a token, not a name */
    ZKT_CHECK(zu_codec_lookup("GZIP") == ZU_CODEC_NONE);      /* names are exact */
    ZKT_CHECK(zu_codec_lookup("") == ZU_CODEC_NONE);
    ZKT_CHECK(zu_codec_available(ZU_CODEC_NONE) == 0);
    ZKT_CHECK(zu_int_registry_lookup(ZU_CODEC_NONE) == NULL);
    ZKT_CHECK(zu_int_registry_count() == ZKT_N_CODECS);
    ZKT_CHECK(zu_int_registry_at(ZKT_N_CODECS) == NULL);
    for (size_t i = 0; i < ZKT_N_CODECS; i++) {
        ZKT_CHECK(zu_int_registry_at(i) != NULL &&
                  zu_int_registry_at(i)->codec == (uint32_t) zkt_codecs[i]);
    }

    /* HTTP content-codings: case-insensitive, and `deflate` is zlib. */
    ZKT_CHECK(zu_codec_from_content_encoding("gzip") == ZU_CODEC_GZIP);
    ZKT_CHECK(zu_codec_from_content_encoding("GZip") == ZU_CODEC_GZIP);
    ZKT_CHECK(zu_codec_from_content_encoding("deflate") == ZU_CODEC_ZLIB);
    ZKT_CHECK(zu_codec_from_content_encoding("DEFLATE") == ZU_CODEC_ZLIB);
    ZKT_CHECK(zu_codec_from_content_encoding("br") == ZU_CODEC_BROTLI);
    ZKT_CHECK(zu_codec_from_content_encoding("zstd") == ZU_CODEC_ZSTD);
    ZKT_CHECK(zu_codec_from_content_encoding("identity") == ZU_CODEC_IDENTITY);
    ZKT_CHECK(zu_codec_from_content_encoding("gzi") == ZU_CODEC_NONE);
    ZKT_CHECK(zu_codec_from_content_encoding("gzipx") == ZU_CODEC_NONE);
    ZKT_CHECK(zu_codec_from_content_encoding("lz4-frame") == ZU_CODEC_NONE);
    ZKT_CHECK(zu_codec_from_content_encoding("") == ZU_CODEC_NONE);
    ZKT_CHECK(zu_codec_from_content_encoding(NULL) == ZU_CODEC_NONE);
    /* '[' is 'Z' + 1 and '@' is 'A' - 1: the ASCII fold is exactly A-Z. */
    ZKT_CHECK(zu_codec_from_content_encoding("gzi@") == ZU_CODEC_NONE);
    ZKT_CHECK(zu_codec_from_content_encoding("zst[") == ZU_CODEC_NONE);
}

static void zkt_info(void)
{
    static const struct {
        zu_codec codec; const char *name; const char *ce;
        int32_t min, max, def, fast, best; uint32_t flags; int detectable;
    } t[] = {
        { ZU_CODEC_IDENTITY, "identity", "identity", 0, 0, 0, 0, 0, 7, 0 },
        { ZU_CODEC_DEFLATE_RAW, "deflate-raw", NULL, 0, 9, 6, 1, 9, 7, 0 },
        { ZU_CODEC_ZLIB, "zlib", "deflate", 0, 9, 6, 1, 9, 7, 1 },
        { ZU_CODEC_GZIP, "gzip", "gzip", 0, 9, 6, 1, 9, 7, 1 },
        { ZU_CODEC_ZSTD, "zstd", "zstd", -131072, 22, 3, 1, 19, 7, 1 },
        { ZU_CODEC_LZ4_FRAME, "lz4-frame", NULL, -65536, 12, 1, 1, 12, 7, 1 },
        { ZU_CODEC_LZ4_BLOCK, "lz4-block", NULL, -65536, 12, 1, 1, 12, 3, 0 }
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        zu_codec_info info;
        memset(&info, 0xEE, sizeof(info));
        info.struct_size = (uint32_t) sizeof(info);
        ZKT_STATUS(zu_codec_get_info(t[i].codec, &info), ZU_OK);
        ZKT_CHECK(info.codec == t[i].codec);
        ZKT_CHECK(info.name != NULL && strcmp(info.name, t[i].name) == 0);
        ZKT_CHECK(t[i].ce == NULL ? info.content_encoding == NULL
                                  : (info.content_encoding != NULL &&
                                     strcmp(info.content_encoding, t[i].ce) == 0));
        ZKT_CHECK(info.source != NULL && strcmp(info.source, "zukomp") == 0);
        ZKT_CHECK(info.level_min == t[i].min);
        ZKT_CHECK(info.level_max == t[i].max);
        ZKT_CHECK(info.level_default == t[i].def);
        ZKT_CHECK(info.level_fast == t[i].fast);
        ZKT_CHECK(info.level_best == t[i].best);
        ZKT_CHECK(info.flags == t[i].flags);
        ZKT_CHECK(info.detectable == t[i].detectable);
    }

    /* A caller compiled before level_fast/level_best existed: its struct
       ends at `detectable`, and nothing past that may be written. */
    struct { zu_codec_info info; } big;
    memset(&big, 0xEE, sizeof(big));
    big.info.struct_size = (uint32_t) (offsetof(zu_codec_info, detectable) + sizeof(int));
    ZKT_STATUS(zu_codec_get_info(ZU_CODEC_ZSTD, &big.info), ZU_OK);
    ZKT_CHECK(big.info.level_default == 3);
    int32_t canary;
    memset(&canary, 0xEE, sizeof(canary));
    ZKT_CHECK(big.info.level_fast == canary && big.info.level_best == canary);
    big.info.struct_size -= 1;
    ZKT_STATUS(zu_codec_get_info(ZU_CODEC_ZSTD, &big.info), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_codec_get_info(ZU_CODEC_ZSTD, NULL), ZU_ERR_INVALID_ARGUMENT);
    zu_codec_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t) sizeof(info);
    ZKT_STATUS(zu_codec_get_info(ZU_CODEC_BROTLI, &info), ZU_ERR_UNSUPPORTED);
    ZKT_STATUS(zu_codec_get_info(ZU_CODEC_NONE, &info), ZU_ERR_UNSUPPORTED);

    /* The list: count-only, too small, exact. */
    size_t n = 0;
    ZKT_STATUS(zu_codec_list(NULL, 0, &n), ZU_OK);
    ZKT_CHECK(n == ZKT_N_CODECS);
    zu_codec list[ZKT_N_CODECS + 1];
    memset(list, 0, sizeof(list));
    n = 0;
    ZKT_STATUS(zu_codec_list(list, ZKT_N_CODECS - 1, &n), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_codec_list(list, ZKT_N_CODECS, NULL), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_codec_list(list, ZKT_N_CODECS, &n), ZU_OK);
    ZKT_CHECK(n == ZKT_N_CODECS);
    for (size_t i = 0; i < ZKT_N_CODECS; i++) {
        ZKT_CHECK(list[i] == zkt_codecs[i]);
    }
    ZKT_CHECK(list[ZKT_N_CODECS] == ZU_CODEC_NONE);
}

static void zkt_sniff(void)
{
    zu_codec out = ZU_CODEC_IDENTITY;
    static const uint8_t gz[] = { 0x1f, 0x8b, 0x08, 0x00 };
    static const uint8_t zs[] = { 0x28, 0xb5, 0x2f, 0xfd, 0x00 };
    static const uint8_t lz[] = { 0x04, 0x22, 0x4d, 0x18, 0x00 };
    static const uint8_t zl[] = { 0x78, 0x9c };
    static const uint8_t zl_bad_check[] = { 0x78, 0x9d };
    static const uint8_t zl_dict[] = { 0x78, 0xbb };     /* FDICT set */
    static const uint8_t zl_cm7[] = { 0x77, 0x9c };
    static const uint8_t junk[] = { 'h', 'e', 'l', 'l', 'o' };

    ZKT_STATUS(zu_sniff(gz, sizeof(gz), &out), ZU_OK);
    ZKT_CHECK(out == ZU_CODEC_GZIP);
    ZKT_STATUS(zu_sniff(zs, sizeof(zs), &out), ZU_OK);
    ZKT_CHECK(out == ZU_CODEC_ZSTD);
    ZKT_STATUS(zu_sniff(lz, sizeof(lz), &out), ZU_OK);
    ZKT_CHECK(out == ZU_CODEC_LZ4_FRAME);
    ZKT_STATUS(zu_sniff(zl, sizeof(zl), &out), ZU_OK);
    ZKT_CHECK(out == ZU_CODEC_ZLIB);
    /* Exactly the magic, and one byte short of it. */
    ZKT_STATUS(zu_sniff(zs, 4, &out), ZU_OK);
    ZKT_CHECK(out == ZU_CODEC_ZSTD);
    ZKT_STATUS(zu_sniff(zs, 3, &out), ZU_ERR_UNSUPPORTED);
    ZKT_CHECK(out == ZU_CODEC_NONE);
    ZKT_STATUS(zu_sniff(lz, 3, &out), ZU_ERR_UNSUPPORTED);
    ZKT_STATUS(zu_sniff(gz, 1, &out), ZU_ERR_UNSUPPORTED);
    ZKT_STATUS(zu_sniff(zl_bad_check, 2, &out), ZU_ERR_UNSUPPORTED);
    /* FDICT is still zlib: detected, then refused when decoded. */
    ZKT_STATUS(zu_sniff(zl_dict, 2, &out), ZU_OK);
    ZKT_CHECK(out == ZU_CODEC_ZLIB);
    ZKT_STATUS(zu_sniff(zl_cm7, 2, &out), ZU_ERR_UNSUPPORTED);
    ZKT_STATUS(zu_sniff(zl, 1, &out), ZU_ERR_UNSUPPORTED);
    ZKT_STATUS(zu_sniff(junk, sizeof(junk), &out), ZU_ERR_UNSUPPORTED);
    ZKT_CHECK(out == ZU_CODEC_NONE);
    ZKT_STATUS(zu_sniff(NULL, 0, &out), ZU_ERR_UNSUPPORTED);
    ZKT_STATUS(zu_sniff(NULL, 1, &out), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_sniff(gz, sizeof(gz), NULL), ZU_ERR_INVALID_ARGUMENT);

    /* Every valid zlib header pair is detected, and only those: CM 8, CINFO
       at most 7, FCHECK a multiple of 31 (RFC 1950 2.2). FDICT does not
       matter to detection. */
    for (unsigned a = 0; a < 256; a++) {
        for (unsigned b = 0; b < 256; b++) {
            uint8_t h[2] = { (uint8_t) a, (uint8_t) b };
            int valid = (a & 0x0f) == 8 && (a >> 4) <= 7 &&
                        ((a << 8) | b) % 31 == 0;
            zu_codec got = ZU_CODEC_NONE;
            zu_status st = zu_sniff(h, 2, &got);
            int as_zlib = (st == ZU_OK && got == ZU_CODEC_ZLIB);
            if (as_zlib != valid) {
                zkt_failures++;
                fprintf(stderr, "test_api.c: zlib sniff of %02x %02x is %d, want %d\n",
                        a, b, as_zlib, valid);
            }
        }
    }
    zkt_checks++;
}

/* -- registration -------------------------------------------------------- */

/* A vendor codec: identity with a magic, so registering it makes something
   detectable and decodable that was not before. */
static zu_status xv_new_e(void **st, const zu_encoder_opts *o) { (void) o; *st = NULL; return ZU_OK; }
static zu_status xv_new_d(void **st, const zu_decoder_opts *o) { (void) o; *st = NULL; return ZU_OK; }
static void      xv_free(void *st) { (void) st; }
static zu_status xv_bound(int32_t level, size_t n, size_t *out) { (void) level; *out = n + 4; return ZU_OK; }

static zu_status xv_copy(zu_buffer *buf, zu_flush flush, int skip_magic)
{
    (void) skip_magic;
    size_t in  = buf->src_size - buf->src_pos;
    size_t out = buf->dst_size - buf->dst_pos;
    size_t n = in < out ? in : out;
    if (n > 0) {
        memcpy(buf->dst + buf->dst_pos, buf->src + buf->src_pos, n);
    }
    buf->src_pos += n;
    buf->dst_pos += n;
    if (flush == ZU_FINISH && buf->src_pos == buf->src_size) {
        return ZU_STREAM_END;
    }
    return buf->src_pos == buf->src_size ? ZU_NEED_INPUT : ZU_NEED_OUTPUT;
}
static zu_status xv_proc(void *st, zu_buffer *buf, zu_flush flush) { (void) st; return xv_copy(buf, flush, 0); }

/* Identity again, but one that asks for more room whenever it fills its
   window, even when it has nothing left: the contract lets a codec be that
   conservative ("progress possible with more output room"), and it is the
   driver's zero-room call that then tells "finished at the cap" from "would
   exceed it". */
static zu_status xv_lazy(void *st, zu_buffer *buf, zu_flush flush)
{
    (void) st;
    size_t before = buf->dst_pos;
    zu_status s = xv_copy(buf, flush, 0);
    if (buf->dst_pos > before && buf->dst_pos == buf->dst_size) {
        return ZU_NEED_OUTPUT;
    }
    return s;
}

static const uint8_t xv_magic[4] = { 'Z', 'K', 'T', '!' };

static zu_codec_vtable zkt_vendor(uint32_t id, const char *name, const char *ce)
{
    zu_codec_vtable v;
    memset(&v, 0, sizeof(v));
    v.struct_size = (uint32_t) sizeof(v);
    v.codec = id;
    v.name = name;
    v.content_encoding = ce;
    v.source = "zkt";
    v.flags = ZU_CAN_ENCODE | ZU_CAN_DECODE;
    v.encoder_new = xv_new_e;
    v.encoder_process = xv_proc;
    v.encoder_free = xv_free;
    v.decoder_new = xv_new_d;
    v.decoder_process = xv_proc;
    v.decoder_free = xv_free;
    v.bound = xv_bound;
    return v;
}

static void zkt_register(void)
{
    uint64_t gen = zu_int_registry_generation();
    ZKT_STATUS(zu_register_codec(NULL), ZU_ERR_INVALID_ARGUMENT);

    /* #75: a vtable shorter than the required prefix, as a real short
       allocation -- the bytes past struct_size do not exist, so nothing
       there may be read. Under ASan (make -C ctest asan) reading `name` or
       `source` first is a heap-buffer-overflow. */
    uint32_t *tiny = malloc(sizeof(uint32_t));
    ZKT_CHECK(tiny != NULL);
    if (tiny != NULL) {
        *tiny = (uint32_t) sizeof(uint32_t);
        ZKT_STATUS(zu_register_codec((const zu_codec_vtable *) (void *) tiny),
                   ZU_ERR_INVALID_ARGUMENT);
        free(tiny);
    }
    /* One byte short of the required prefix, also a real allocation. */
    zu_codec_vtable full = zkt_vendor(2001, "zkt-short", NULL);
    size_t req = ZU_VTABLE_REQUIRED_SIZE;
    uint8_t *shrt = malloc(req - 1);
    ZKT_CHECK(shrt != NULL);
    if (shrt != NULL) {
        memcpy(shrt, &full, req - 1);
        uint32_t size = (uint32_t) (req - 1);
        memcpy(shrt, &size, sizeof(size));      /* struct_size is first */
        ZKT_STATUS(zu_register_codec((const zu_codec_vtable *) (void *) shrt),
                   ZU_ERR_INVALID_ARGUMENT);
        free(shrt);
    }

    zu_codec_vtable v;
#define ZKT_REJECT(mutation)                                               \
    do {                                                                   \
        v = zkt_vendor(2000, "zkt-vendor", "x-zkt");                       \
        mutation;                                                          \
        ZKT_STATUS(zu_register_codec(&v), ZU_ERR_INVALID_ARGUMENT);        \
    } while (0)

    ZKT_REJECT(v.name = NULL);
    ZKT_REJECT(v.source = NULL);
    ZKT_REJECT(v.codec = 0);
    ZKT_REJECT(v.codec = 1023);                     /* below the vendor base */
    ZKT_REJECT(v.codec = 22);                       /* reserved, undeclared */
    ZKT_REJECT(v.codec = ZU_CODEC_GZIP);            /* already registered */
    ZKT_REJECT(v.codec = ZU_CODEC_BROTLI);          /* declared: wrong name */
    ZKT_REJECT(v.name = "gzip");                    /* a declared name */
    ZKT_REJECT(v.name = "brotli");                  /* reserved for brotli's id */
    ZKT_REJECT(v.content_encoding = "GZIP");        /* a declared token */
    ZKT_REJECT(v.content_encoding = "br");          /* reserved for brotli */
    ZKT_REJECT(v.encoder_new = NULL);
    ZKT_REJECT(v.encoder_process = NULL);
    ZKT_REJECT(v.encoder_free = NULL);
    ZKT_REJECT(v.decoder_new = NULL);
    ZKT_REJECT(v.decoder_process = NULL);
    ZKT_REJECT(v.decoder_free = NULL);
    ZKT_REJECT(v.struct_size = (uint32_t) ZU_VTABLE_REQUIRED_SIZE - 1);
    /* A declared codec claiming another's token: lz4-frame is registered
       already, so use brotli's id with its own name and gzip's token. */
    ZKT_REJECT((v.codec = ZU_CODEC_BROTLI, v.name = "brotli", v.content_encoding = "gzip"));
    ZKT_CHECK(zu_int_registry_generation() == gen);
    ZKT_CHECK(zu_int_registry_count() == ZKT_N_CODECS);

    /* A direction the vtable does not claim needs no functions for it. */
    static zu_codec_vtable dec_only;
    dec_only = zkt_vendor(2002, "zkt-decode-only", NULL);
    dec_only.flags = ZU_CAN_DECODE;
    dec_only.encoder_new = NULL;
    dec_only.encoder_process = NULL;
    dec_only.encoder_free = NULL;
    ZKT_STATUS(zu_register_codec(&dec_only), ZU_OK);
    ZKT_CHECK(zu_int_registry_generation() == gen + 1);
    zu_encoder_opts eo;
    memset(&eo, 0, sizeof(eo));
    eo.struct_size = (uint32_t) sizeof(eo);
    eo.codec = (zu_codec) 2002;
    eo.level = ZU_LEVEL_DEFAULT;
    zu_encoder *e = NULL;
    ZKT_STATUS(zu_encoder_new(&e, &eo), ZU_ERR_UNSUPPORTED);
    size_t bound;
    ZKT_STATUS(zu_compress_bound((zu_codec) 2002, ZU_LEVEL_DEFAULT, 1, &bound),
               ZU_ERR_UNSUPPORTED);

    /* A vendor codec with a magic and a token: found by name, token and
       content, and the core's limits bind on it. */
    static zu_codec_vtable vend;
    vend = zkt_vendor(2000, "zkt-vendor", "x-zkt");
    vend.magic = xv_magic;
    vend.magic_len = 4;
    ZKT_STATUS(zu_register_codec(&vend), ZU_OK);
    ZKT_CHECK(zu_int_registry_generation() == gen + 2);
    ZKT_CHECK(zu_codec_lookup("zkt-vendor") == (zu_codec) 2000);
    ZKT_CHECK(zu_codec_from_content_encoding("X-ZKT") == (zu_codec) 2000);
    ZKT_CHECK(zu_codec_available((zu_codec) 2000) == 1);
    ZKT_STATUS(zu_register_codec(&vend), ZU_ERR_INVALID_ARGUMENT);
    v = zkt_vendor(2003, "zkt-vendor", NULL);       /* its name, a new id */
    ZKT_STATUS(zu_register_codec(&v), ZU_ERR_INVALID_ARGUMENT);
    v = zkt_vendor(2003, "zkt-other", "x-ZKT");     /* its token */
    ZKT_STATUS(zu_register_codec(&v), ZU_ERR_INVALID_ARGUMENT);

    uint8_t payload[12] = { 'Z', 'K', 'T', '!', 1, 2, 3, 4, 5, 6, 7, 8 };
    zu_codec seen = ZU_CODEC_NONE;
    ZKT_STATUS(zu_sniff(payload, sizeof(payload), &seen), ZU_OK);
    ZKT_CHECK(seen == (zu_codec) 2000);
    zu_codec_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t) sizeof(info);
    ZKT_STATUS(zu_codec_get_info((zu_codec) 2000, &info), ZU_OK);
    ZKT_CHECK(info.detectable == 1 && strcmp(info.source, "zkt") == 0);
    /* No levels advertised: fast and best fall back to the default. */
    ZKT_CHECK(info.level_fast == 0 && info.level_best == 0);

    zkt_drive_opts o = zkt_defaults();
    o.max_output = 11;
    zkt_bytes got = {0};
    ZKT_STATUS(zkt_decode((zu_codec) 2000, payload, sizeof(payload), &o, &got, NULL),
               ZU_ERR_OUTPUT_LIMIT);
    ZKT_CHECK(got.n == 11);
    o.max_output = 12;
    got.n = 0;
    ZKT_STATUS(zkt_decode((zu_codec) 2000, payload, sizeof(payload), &o, &got, NULL),
               ZU_OK);
    ZKT_CHECK(zkt_bytes_eq(&got, payload, sizeof(payload)));
    zkt_bytes_free(&got);

    /* The driver gives a codec that filled its window exactly one call with
       no room to finish in, so a stream ending at the cap passes when the
       window lands on the cap. `remaining < allowance` in
       zu_decoder_process() is what decides that; at `<=` the window that
       lands exactly on the cap is treated as a shrunken one and refused
       (#66). The built-in codecs never ask for room they do not need, so
       only a codec like this one can tell the two apart. */
    static zu_codec_vtable lazy;
    lazy = zkt_vendor(2004, "zkt-lazy", NULL);
    lazy.decoder_process = xv_lazy;
    ZKT_STATUS(zu_register_codec(&lazy), ZU_OK);
    static const size_t windows[] = { 12, 6, 4, 3 };
    for (size_t i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        zkt_drive_opts lo = zkt_defaults();
        lo.out_chunk = windows[i];
        lo.max_output = sizeof(payload);
        zkt_bytes lg = {0};
        ZKT_STATUS(zkt_decode((zu_codec) 2004, payload, sizeof(payload), &lo, &lg, NULL),
                   ZU_OK);
        ZKT_CHECK(zkt_bytes_eq(&lg, payload, sizeof(payload)));
        lg.n = 0;
        lo.max_output = sizeof(payload) - 1;
        ZKT_STATUS(zkt_decode((zu_codec) 2004, payload, sizeof(payload), &lo, &lg, NULL),
                   ZU_ERR_OUTPUT_LIMIT);
        ZKT_CHECK(lg.n == sizeof(payload) - 1);
        zkt_bytes_free(&lg);
    }

    /* The level helpers on a vtable that predates the fields. */
    zu_codec_vtable old = zkt_vendor(3000, "old", NULL);
    old.level_min = 1;
    old.level_max = 5;
    old.level_default = 3;
    old.level_fast = 1;
    old.level_best = 5;
    ZKT_CHECK(zu_int_vtable_level_fast(&old) == 1);
    ZKT_CHECK(zu_int_vtable_level_best(&old) == 5);
    old.struct_size = (uint32_t) (offsetof(zu_codec_vtable, level_best) + sizeof(int32_t) - 1);
    ZKT_CHECK(zu_int_vtable_level_fast(&old) == 3);
    ZKT_CHECK(zu_int_vtable_level_best(&old) == 3);
    old.struct_size = (uint32_t) sizeof(old);
    old.level_fast = 0;
    old.level_best = 0;
    ZKT_CHECK(zu_int_vtable_level_fast(&old) == 3);
    ZKT_CHECK(zu_int_vtable_level_best(&old) == 3);
    old.level_best = 4;                 /* one advertised is advertised */
    ZKT_CHECK(zu_int_vtable_level_fast(&old) == 0);
    ZKT_CHECK(zu_int_vtable_level_best(&old) == 4);

    /* The registry is a fixed table of 32: the 33rd registration is
       ZU_ERR_MEMORY, not an overrun. */
    static zu_codec_vtable many[40];
    static char names[40][16];
    size_t ok = 0;
    zu_status last = ZU_OK;
    for (size_t i = 0; i < 40; i++) {
        snprintf(names[i], sizeof(names[i]), "zkt-%zu", i);
        many[i] = zkt_vendor((uint32_t) (4000 + i), names[i], NULL);
        last = zu_register_codec(&many[i]);
        if (last != ZU_OK) {
            break;
        }
        ok++;
    }
    ZKT_STATUS(last, ZU_ERR_MEMORY);
    ZKT_CHECK(zu_int_registry_count() == 32);
    ZKT_CHECK(ok == 32 - ZKT_N_CODECS - 3);
    size_t n = 0;
    ZKT_STATUS(zu_codec_list(NULL, 0, &n), ZU_OK);
    ZKT_CHECK(n == 32);
#undef ZKT_REJECT
}

void zkt_suite_api(void)
{
    zkt_status_strings();
    zkt_lookup();
    zkt_info();
    zkt_sniff();
}

void zkt_suite_registry(void)
{
    zkt_register();
}
