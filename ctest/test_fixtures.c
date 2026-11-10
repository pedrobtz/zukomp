/* The committed interop and conformance corpora, decoded without R.
 *
 * tests/testthat/fixtures/ holds streams written by external encoders
 * (python zlib, gzip(1), zstd(1), lz4(1), R's memCompress) and, under
 * malformed/, DEFLATE streams built field by field from RFC 1951 with a
 * declared outcome. The R suite reads them through komp_decompress(); this
 * reads them through the C driver, so a mutant in a decoder is judged by
 * bytes another implementation wrote rather than by a round trip through the
 * same code. The payloads are rebuilt here byte for byte from the
 * definitions in helper-corpus.R, which the fixture generators source.
 *
 * Every file named in a manifest must be found. A missing corpus is a
 * failure, never a skip: a skip reachable from "the files are not there"
 * would pass with nothing tested. */
#include "zkt.h"

static const char *zkt_fixture_dir(void)
{
    const char *env = getenv("ZKT_FIXTURES");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    /* From the repository root (mull-runner, CI) or from ctest/ (make). */
    static const char *const tries[] = {
        "tests/testthat/fixtures", "../tests/testthat/fixtures"
    };
    for (size_t i = 0; i < 2; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/MANIFEST.tsv", tries[i]);
        FILE *f = fopen(path, "rb");
        if (f != NULL) {
            fclose(f);
            return tries[i];
        }
    }
    return NULL;
}

static int zkt_read_file(const char *path, zkt_bytes *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    uint8_t chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        zkt_bytes_put(out, chunk, got);
    }
    fclose(f);
    return 1;
}

/* -- helper-corpus.R's new_payload(), in C ------------------------------- */

static void zkt_payload(const char *kind, size_t n, zkt_bytes *out)
{
    if (strcmp(kind, "empty") == 0) {
        return;
    }
    if (strcmp(kind, "zeros") == 0) {
        for (size_t i = 0; i < n; i++) {
            uint8_t z = 0;
            zkt_bytes_put(out, &z, 1);
        }
        return;
    }
    if (strcmp(kind, "ascii") == 0) {
        static const char unit[] = "the quick brown fox jumps. ";
        for (size_t i = 0; i < n; i++) {
            zkt_bytes_put(out, (const uint8_t *) &unit[i % (sizeof(unit) - 1)], 1);
        }
        return;
    }
    if (strcmp(kind, "utf8") == 0) {
        static const uint8_t unit[] = {
            0x63, 0x61, 0x66, 0xc3, 0xa9, 0x20,
            0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87, 0x20,
            0xc3, 0xbc, 0x62, 0x65, 0x72, 0x20
        };
        size_t reps = n / sizeof(unit);
        for (size_t r = 0; r < reps; r++) {
            zkt_bytes_put(out, unit, sizeof(unit));
        }
        while (out->n < n) {
            uint8_t sp = 0x20;
            zkt_bytes_put(out, &sp, 1);
        }
        return;
    }
    if (strcmp(kind, "lcg") == 0) {
        uint64_t x = 20260907u;
        for (size_t i = 0; i < n; i++) {
            x = (16807u * x) % 2147483647u;
            uint8_t b = (uint8_t) ((x / 128u) % 256u);
            zkt_bytes_put(out, &b, 1);
        }
        return;
    }
    if (strcmp(kind, "structured") == 0) {
        size_t m = n / 16u;
        if (m < 1) {
            m = 1;
        }
        for (size_t i = 1; i <= m && out->n < n; i++) {
            char item[64];
            int len = snprintf(item, sizeof(item), "%s{\"id\":%zu,\"v\":\"x\"}",
                               i > 1 ? "," : "", i);
            zkt_bytes_put(out, (const uint8_t *) item, (size_t) len);
        }
        if (out->n > n) {
            out->n = n;
        }
        return;
    }
    fprintf(stderr, "zkt: unknown payload kind '%s'\n", kind);
    zkt_failures++;
}

/* -- a minimal TSV reader ------------------------------------------------ */

#define ZKT_MAX_COLS 16

typedef struct {
    char *cell[ZKT_MAX_COLS];
    int   n;
} zkt_row;

/* Splits `line` in place on tabs, stripping the newline. */
static void zkt_split(char *line, zkt_row *row)
{
    row->n = 0;
    line[strcspn(line, "\r\n")] = '\0';
    char *p = line;
    while (row->n < ZKT_MAX_COLS) {
        row->cell[row->n++] = p;
        char *tab = strchr(p, '\t');
        if (tab == NULL) {
            break;
        }
        *tab = '\0';
        p = tab + 1;
    }
}

static int zkt_col(const zkt_row *header, const char *name)
{
    for (int i = 0; i < header->n; i++) {
        if (strcmp(header->cell[i], name) == 0) {
            return i;
        }
    }
    fprintf(stderr, "zkt: manifest has no column '%s'\n", name);
    zkt_failures++;
    return -1;
}

static const char *zkt_cell(const zkt_row *row, int col)
{
    return (col >= 0 && col < row->n) ? row->cell[col] : "";
}

/* -- the interop corpus -------------------------------------------------- */

static void zkt_interop(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/MANIFEST.tsv", dir);
    FILE *f = fopen(path, "rb");
    ZKT_CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    char hline[1024], line[1024];
    zkt_row header, row;
    if (fgets(hline, sizeof(hline), f) == NULL) {
        fclose(f);
        ZKT_CHECK(!"interop manifest is empty");
        return;
    }
    zkt_split(hline, &header);
    int c_codec = zkt_col(&header, "codec"), c_file = zkt_col(&header, "file");
    int c_kind = zkt_col(&header, "payload"), c_n = zkt_col(&header, "n");
    int c_members = zkt_col(&header, "members");

    int rows = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        zkt_split(line, &row);
        if (row.n < 2) {
            continue;
        }
        rows++;
        const char *cname = zkt_cell(&row, c_codec);
        zu_codec codec = zu_codec_lookup(cname);
        ZKT_CHECK(codec != ZU_CODEC_NONE);

        zkt_bytes in = {0}, want = {0}, one = {0};
        snprintf(path, sizeof(path), "%s/%s/%s", dir, cname,
                 zkt_cell(&row, c_file));
        int found = zkt_read_file(path, &in);
        if (!found) {
            fprintf(stderr, "zkt: missing fixture %s\n", path);
        }
        ZKT_CHECK(found);

        size_t n = (size_t) strtoul(zkt_cell(&row, c_n), NULL, 10);
        long members = strtol(zkt_cell(&row, c_members), NULL, 10);
        zkt_payload(zkt_cell(&row, c_kind), n, &one);
        for (long m = 0; m < members; m++) {
            zkt_bytes_put(&want, one.p, one.n);
        }

        /* Whole, and at one byte in and one byte out: the harshest
           boundary, and where the wrapper and frame state machines are
           fed a byte at a time. Plus the trailing policy and the exact
           cursor: a decoder must take every byte of a valid stream. */
        static const size_t chunks[][2] = { {1u << 20, 1u << 20}, {1, 1}, {7, 3} };
        for (size_t k = 0; k < 3; k++) {
            zkt_drive_opts o = zkt_defaults();
            o.in_chunk  = chunks[k][0];
            o.out_chunk = chunks[k][1];
            o.flags = ZU_DEC_REJECT_TRAILING | ZU_DEC_CONCAT_MEMBERS;
            zkt_bytes got = {0};
            size_t consumed = 0;
            zu_status st = zkt_decode(codec, in.p, in.n, &o, &got, &consumed);
            ZKT_STATUS(st, ZU_OK);
            ZKT_CHECK(zkt_bytes_eq(&got, want.p, want.n));
            ZKT_CHECK(consumed == in.n);
            if (st != ZU_OK || !zkt_bytes_eq(&got, want.p, want.n)) {
                fprintf(stderr, "  fixture %s at chunks %zu/%zu\n", path,
                        chunks[k][0], chunks[k][1]);
            }
            zkt_bytes_free(&got);
        }

        /* Detection: every external stream of a detectable codec is
           detected as that codec. */
        zu_codec_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t) sizeof(info);
        if (zu_codec_get_info(codec, &info) == ZU_OK && info.detectable &&
            in.n > 0) {
            zu_codec seen = ZU_CODEC_NONE;
            ZKT_STATUS(zu_sniff(in.p, in.n, &seen), ZU_OK);
            ZKT_CHECK(seen == codec);
        }

        zkt_bytes_free(&in);
        zkt_bytes_free(&want);
        zkt_bytes_free(&one);
    }
    fclose(f);
    /* A manifest that lost its rows must not pass as "nothing failed". */
    ZKT_CHECK(rows >= 50);
}

/* -- the malformed DEFLATE corpus ----------------------------------------- */

static zu_status zkt_class_status(const char *cls)
{
    if (strcmp(cls, "zukomp_invalid_data") == 0)   return ZU_ERR_INVALID_DATA;
    if (strcmp(cls, "zukomp_truncated") == 0)      return ZU_ERR_TRUNCATED;
    if (strcmp(cls, "zukomp_checksum_error") == 0) return ZU_ERR_CHECKSUM;
    if (strcmp(cls, "zukomp_trailing_bytes") == 0) return ZU_ERR_TRAILING;
    if (strcmp(cls, "zukomp_output_limit") == 0)   return ZU_ERR_OUTPUT_LIMIT;
    return ZU_ERR_INTERNAL;
}

static void zkt_malformed(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/malformed/MANIFEST.tsv", dir);
    FILE *f = fopen(path, "rb");
    ZKT_CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    char hline[2048], line[8192];
    zkt_row header, row;
    if (fgets(hline, sizeof(hline), f) == NULL) {
        fclose(f);
        ZKT_CHECK(!"malformed manifest is empty");
        return;
    }
    zkt_split(hline, &header);
    int c_case = zkt_col(&header, "case"), c_codec = zkt_col(&header, "codec");
    int c_expect = zkt_col(&header, "expect"), c_class = zkt_col(&header, "class");
    int c_n = zkt_col(&header, "output_n"), c_hex = zkt_col(&header, "output_hex");

    int rows = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        zkt_split(line, &row);
        if (row.n < 2) {
            continue;
        }
        rows++;
        zu_codec codec = zu_codec_lookup(zkt_cell(&row, c_codec));
        zkt_bytes in = {0};
        snprintf(path, sizeof(path), "%s/malformed/%s.bin", dir,
                 zkt_cell(&row, c_case));
        ZKT_CHECK(zkt_read_file(path, &in));

        const char *expect = zkt_cell(&row, c_expect);
        /* A cap well above every vector's output: raw-missing-end-of-block
           decodes zero-bit padding as literals for as long as it is
           allowed to, so an uncapped decode would never end. */
        static const size_t chunks[] = { 1u << 20, 1 };
        for (size_t k = 0; k < 2; k++) {
            zkt_drive_opts o = zkt_defaults();
            o.in_chunk = o.out_chunk = chunks[k];
            o.max_output = 1u << 20;
            o.flags = ZU_DEC_REJECT_TRAILING;
            zkt_bytes got = {0};
            zu_status st = zkt_decode(codec, in.p, in.n, &o, &got, NULL);
            if (strcmp(expect, "ok") == 0) {
                ZKT_STATUS(st, ZU_OK);
                size_t want_n = (size_t) strtoul(zkt_cell(&row, c_n), NULL, 10);
                ZKT_CHECK(got.n == want_n);
                const char *hex = zkt_cell(&row, c_hex);
                ZKT_CHECK(strlen(hex) == 2 * want_n);
                for (size_t i = 0; i < want_n && i < got.n && 2 * i + 1 < strlen(hex); i++) {
                    char byte[3] = { hex[2 * i], hex[2 * i + 1], '\0' };
                    ZKT_CHECK(got.p[i] == (uint8_t) strtoul(byte, NULL, 16));
                }
            } else if (strcmp(zkt_cell(&row, c_case), "raw-missing-end-of-block") == 0) {
                /* Zero-bit padding decodes as literals for as long as the
                   output allows, so this one is classified by delivery: the
                   cap or the data, depending on where it stops (#74). It
                   must fail either way. */
                ZKT_CHECK(st == ZU_ERR_INVALID_DATA || st == ZU_ERR_OUTPUT_LIMIT);
            } else {
                ZKT_STATUS(st, zkt_class_status(zkt_cell(&row, c_class)));
            }
            if (st != ZU_OK && strcmp(expect, "ok") == 0) {
                fprintf(stderr, "  vector %s\n", zkt_cell(&row, c_case));
            }
            zkt_bytes_free(&got);
        }
        zkt_bytes_free(&in);
    }
    fclose(f);
    ZKT_CHECK(rows >= 16);
}

void zkt_suite_fixtures(void)
{
    const char *dir = zkt_fixture_dir();
    if (dir == NULL) {
        fprintf(stderr, "zkt: tests/testthat/fixtures not found; "
                        "run from the repository root or ctest/, "
                        "or set ZKT_FIXTURES\n");
    }
    ZKT_CHECK(dir != NULL);
    if (dir == NULL) {
        return;
    }
    zkt_interop(dir);
    zkt_malformed(dir);
}
