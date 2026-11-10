/* The gzip header parser, fed a byte at a time as the decoder feeds it, and
 * the checked size arithmetic every buffer computation goes through. */
#include "zkt.h"

#include "miniz.h"

#define FTEXT    0x01
#define FHCRC    0x02
#define FEXTRA   0x04
#define FNAME    0x08
#define FCOMMENT 0x10

/* Builds an RFC 1952 header with the given optional fields. */
static void zkt_gzip_header(uint8_t flg, uint16_t xlen, const char *name,
                            const char *comment, int bad_hcrc, zkt_bytes *out)
{
    uint8_t fixed[10] = { 0x1f, 0x8b, 8, flg, 0x12, 0x34, 0x56, 0x78, 2, 3 };
    zkt_bytes_put(out, fixed, sizeof(fixed));
    if (flg & FEXTRA) {
        uint8_t xl[2] = { (uint8_t) (xlen & 0xff), (uint8_t) (xlen >> 8) };
        zkt_bytes_put(out, xl, 2);
        for (uint16_t i = 0; i < xlen; i++) {
            uint8_t b = (uint8_t) (i * 7);
            zkt_bytes_put(out, &b, 1);
        }
    }
    if (flg & FNAME) {
        zkt_bytes_put(out, (const uint8_t *) name, strlen(name) + 1);
    }
    if (flg & FCOMMENT) {
        zkt_bytes_put(out, (const uint8_t *) comment, strlen(comment) + 1);
    }
    if (flg & FHCRC) {
        mz_ulong crc = mz_crc32(MZ_CRC32_INIT, out->p, out->n);
        uint8_t h[2] = { (uint8_t) (crc & 0xff), (uint8_t) ((crc >> 8) & 0xff) };
        if (bad_hcrc) {
            h[0] ^= 1;
        }
        zkt_bytes_put(out, h, 2);
    }
}

/* Feeds the header; returns the status of the first byte that is not
   ZU_OK, or ZU_OK, with *at the number of bytes fed when `done` was set
   (or when it failed). */
static zu_status zkt_feed(const zkt_bytes *h, size_t *at)
{
    zu_int_gzip_header st;
    zu_int_gzip_header_init(&st);
    for (size_t i = 0; i < h->n; i++) {
        int done = -1;
        zu_status s = zu_int_gzip_header_feed(&st, h->p[i], &done);
        if (s != ZU_OK) {
            *at = i + 1;
            return s;
        }
        if (done) {
            *at = i + 1;
            return ZU_OK;
        }
        if (done != 0) {
            *at = i + 1;
            return ZU_ERR_INTERNAL;     /* done must be written */
        }
    }
    *at = h->n + 1;                     /* never finished */
    return ZU_ERR_TRUNCATED;
}

static void zkt_gzip_parser(void)
{
    static const struct { uint8_t flg; uint16_t xlen; const char *name, *comment; } t[] = {
        { 0, 0, "", "" },
        { FTEXT, 0, "", "" },
        { FEXTRA, 0, "", "" },
        { FEXTRA, 1, "", "" },
        { FEXTRA, 300, "", "" },
        { FNAME, 0, "", "" },
        { FNAME, 0, "file.txt", "" },
        { FCOMMENT, 0, "", "a comment" },
        { FHCRC, 0, "", "" },
        { FEXTRA | FNAME, 5, "n", "" },
        { FNAME | FCOMMENT, 0, "name", "comment" },
        { FEXTRA | FCOMMENT | FHCRC, 2, "", "c" },
        { FEXTRA | FNAME | FCOMMENT | FHCRC, 4, "name.gz", "the comment" },
        { FNAME | FHCRC, 0, "x", "" },
        { FCOMMENT | FHCRC, 0, "", "y" }
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        zkt_bytes h = {0};
        zkt_gzip_header(t[i].flg, t[i].xlen, t[i].name, t[i].comment, 0, &h);
        size_t at = 0;
        /* done exactly on the last header byte, never before. */
        ZKT_STATUS(zkt_feed(&h, &at), ZU_OK);
        ZKT_CHECK(at == h.n);
        if (at != h.n) {
            fprintf(stderr, "  header flags %02x: done at %zu of %zu\n", t[i].flg, at, h.n);
        }

        /* And the whole member decodes, with this header, at one byte per
           call, to the payload. */
        static uint8_t text[700];
        zkt_payload_text(text, sizeof(text));
        zkt_bytes raw = {0}, member = {0}, got = {0};
        zkt_drive_opts o = zkt_defaults();
        ZKT_STATUS(zkt_encode(ZU_CODEC_DEFLATE_RAW, text, sizeof(text), &o, &raw), ZU_OK);
        zkt_bytes_put(&member, h.p, h.n);
        zkt_bytes_put(&member, raw.p, raw.n);
        mz_ulong crc = mz_crc32(MZ_CRC32_INIT, text, sizeof(text));
        uint8_t tr[8] = { (uint8_t) crc, (uint8_t) (crc >> 8), (uint8_t) (crc >> 16),
                          (uint8_t) (crc >> 24), (uint8_t) sizeof(text),
                          (uint8_t) (sizeof(text) >> 8), 0, 0 };
        zkt_bytes_put(&member, tr, 8);
        o.in_chunk = o.out_chunk = 1;
        o.flags = ZU_DEC_REJECT_TRAILING;
        ZKT_STATUS(zkt_decode(ZU_CODEC_GZIP, member.p, member.n, &o, &got, NULL), ZU_OK);
        ZKT_CHECK(zkt_bytes_eq(&got, text, sizeof(text)));

        if (t[i].flg & FHCRC) {
            zkt_bytes bad = {0};
            zkt_gzip_header(t[i].flg, t[i].xlen, t[i].name, t[i].comment, 1, &bad);
            ZKT_STATUS(zkt_feed(&bad, &at), ZU_ERR_CHECKSUM);
            ZKT_CHECK(at == bad.n);
            zkt_bytes_free(&bad);
        }
        /* Every proper prefix of the header is unfinished, not an error. */
        for (size_t cut = 0; cut + 1 < h.n; cut++) {
            zkt_bytes p = {0};
            zkt_bytes_put(&p, h.p, cut);
            ZKT_STATUS(zkt_feed(&p, &at), ZU_ERR_TRUNCATED);
            zkt_bytes_free(&p);
        }
        zkt_bytes_free(&h);
        zkt_bytes_free(&raw);
        zkt_bytes_free(&member);
        zkt_bytes_free(&got);
    }

    /* The fixed fields: each magic byte, CM, and each reserved FLG bit. */
    zkt_bytes h = {0};
    zkt_gzip_header(0, 0, "", "", 0, &h);
    size_t at;
    static const struct { size_t pos; uint8_t val; } bad[] = {
        { 0, 0x1e }, { 1, 0x8a }, { 2, 7 }, { 2, 9 }, { 3, 0x20 }, { 3, 0x40 }, { 3, 0x80 }
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t keep = h.p[bad[i].pos];
        h.p[bad[i].pos] = bad[i].val;
        ZKT_STATUS(zkt_feed(&h, &at), ZU_ERR_INVALID_DATA);
        ZKT_CHECK(at == bad[i].pos + 1);
        h.p[bad[i].pos] = keep;
    }
    zkt_bytes_free(&h);

    /* FNAME is bounded: 65535 bytes pass, 65536 do not. */
    for (size_t extra = 0; extra < 2; extra++) {
        size_t len = ZU_INT_GZIP_MAX_FIELD + extra;
        char *name = malloc(len + 1);
        ZKT_CHECK(name != NULL);
        if (name == NULL) {
            break;
        }
        memset(name, 'n', len);
        name[len] = '\0';
        zkt_bytes hn = {0};
        zkt_gzip_header(FNAME, 0, name, "", 0, &hn);
        ZKT_STATUS(zkt_feed(&hn, &at), extra ? ZU_ERR_INVALID_DATA : ZU_OK);
        ZKT_CHECK(at == (extra ? 10 + len : hn.n));
        zkt_bytes hc = {0};
        zkt_gzip_header(FCOMMENT, 0, "", name, 0, &hc);
        ZKT_STATUS(zkt_feed(&hc, &at), extra ? ZU_ERR_INVALID_DATA : ZU_OK);
        zkt_bytes_free(&hn);
        zkt_bytes_free(&hc);
        free(name);
    }

    /* The writer: the deterministic header gzip output starts with. */
    uint8_t w[ZU_INT_GZIP_HEADER_LEN];
    memset(w, 0xAA, sizeof(w));
    zu_int_gzip_write_header(w);
    static const uint8_t want[ZU_INT_GZIP_HEADER_LEN] = {
        0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff
    };
    ZKT_CHECK(memcmp(w, want, sizeof(want)) == 0);
}

static void zkt_size_arithmetic(void)
{
    size_t out = 12345;
    ZKT_STATUS(zu_int_add(1, 2, NULL), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_int_mul(1, 2, NULL), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_int_grow(1, 2, NULL), ZU_ERR_INVALID_ARGUMENT);

    ZKT_STATUS(zu_int_add(2, 3, &out), ZU_OK);
    ZKT_CHECK(out == 5);
    ZKT_STATUS(zu_int_add(SIZE_MAX, 0, &out), ZU_OK);
    ZKT_CHECK(out == SIZE_MAX);
    ZKT_STATUS(zu_int_add(SIZE_MAX - 5, 5, &out), ZU_OK);
    ZKT_CHECK(out == SIZE_MAX);
    out = 7;
    ZKT_STATUS(zu_int_add(SIZE_MAX - 5, 6, &out), ZU_ERR_MEMORY);
    ZKT_STATUS(zu_int_add(SIZE_MAX, SIZE_MAX, &out), ZU_ERR_MEMORY);

    ZKT_STATUS(zu_int_mul(6, 7, &out), ZU_OK);
    ZKT_CHECK(out == 42);
    ZKT_STATUS(zu_int_mul(0, SIZE_MAX, &out), ZU_OK);
    ZKT_CHECK(out == 0);
    ZKT_STATUS(zu_int_mul(SIZE_MAX, 0, &out), ZU_OK);
    ZKT_CHECK(out == 0);
    ZKT_STATUS(zu_int_mul(SIZE_MAX, 1, &out), ZU_OK);
    ZKT_CHECK(out == SIZE_MAX);
    ZKT_STATUS(zu_int_mul(2, SIZE_MAX / 2, &out), ZU_OK);
    ZKT_CHECK(out == SIZE_MAX - 1);
    ZKT_STATUS(zu_int_mul(2, SIZE_MAX / 2 + 1, &out), ZU_ERR_MEMORY);
    ZKT_STATUS(zu_int_mul(SIZE_MAX / 2 + 1, 2, &out), ZU_ERR_MEMORY);

    /* Growth doubles, never below the floor, and to exactly what is needed
       when doubling would not be enough or would overflow. */
    ZKT_STATUS(zu_int_grow(0, 1, &out), ZU_OK);
    ZKT_CHECK(out == ZU_INT_MIN_BUFFER);
    ZKT_STATUS(zu_int_grow(0, ZU_INT_MIN_BUFFER + 1, &out), ZU_OK);
    ZKT_CHECK(out == ZU_INT_MIN_BUFFER + 1);
    ZKT_STATUS(zu_int_grow(100, 1, &out), ZU_OK);
    ZKT_CHECK(out == ZU_INT_MIN_BUFFER);
    ZKT_STATUS(zu_int_grow(5000, 1, &out), ZU_OK);
    ZKT_CHECK(out == 10000);
    ZKT_STATUS(zu_int_grow(5000, 5000, &out), ZU_OK);
    ZKT_CHECK(out == 10000);
    ZKT_STATUS(zu_int_grow(5000, 5001, &out), ZU_OK);
    ZKT_CHECK(out == 10001);
    ZKT_STATUS(zu_int_grow(SIZE_MAX / 2 + 1, 1, &out), ZU_OK);
    ZKT_CHECK(out == SIZE_MAX / 2 + 2);
    ZKT_STATUS(zu_int_grow(SIZE_MAX, 0, &out), ZU_OK);
    ZKT_CHECK(out == SIZE_MAX);
    ZKT_STATUS(zu_int_grow(SIZE_MAX, 1, &out), ZU_ERR_MEMORY);

    uint8_t x[2];
    ZKT_CHECK(zu_int_cat(NULL, 0) == NULL);
    ZKT_CHECK(zu_int_at(NULL, 0) == NULL);
    ZKT_CHECK(zu_int_cat(x, 1) == x + 1);
    ZKT_CHECK(zu_int_at(x, 1) == x + 1);
}

void zkt_suite_gzip(void)
{
    zkt_gzip_parser();
}

void zkt_suite_buf(void)
{
    zkt_size_arithmetic();
}
