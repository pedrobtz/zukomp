/* The ZIP reader that libzukomp.a ships, as a consumer drives it (#37).
 *
 * zuxlsx links libzukomp.a and reads untrusted .xlsx files through miniz's
 * mz_zip_reader_*, which zukomp.so never compiles. This target builds
 * miniz.c the way src/Makevars builds the archive's miniz_zip.o -- the trim
 * minus ARCHIVE_APIS, STDIO and TIME -- and on its own, not with the core:
 * the core already carries miniz.c under the full trim, and two copies in
 * one link would collide. It lives in fuzz/archive/, outside build.sh's
 * fuzz/fuzz_*.c loop, for that reason; build.sh builds it last.
 *
 * Each input is opened with mz_zip_reader_init_mem(), and every entry is
 * stat'ed, located by name and extracted three ways: through the extract
 * iterator (what xlsxio uses), mz_zip_reader_extract_to_mem() (and so
 * extract_to_heap), and mz_zip_reader_extract_to_callback() (and so
 * extract_to_file and extract_to_cfile, the paths a file reader takes).
 * "No crash" is ASan's and UBSan's half. The oracles are the rest, and an
 * input that breaks one aborts, so libFuzzer records it as a crash:
 *
 *   - a refusal is classed: a call that fails leaves an mz_zip_error other
 *     than MZ_ZIP_NO_ERROR (APPNOTE.TXT does not name errors; this is the
 *     consumer's contract, the one zukomp's own codecs keep through
 *     zu_status, design 7);
 *   - an extraction that reports success delivered exactly the entry's
 *     uncompressed size, and those bytes have the entry's CRC-32 (APPNOTE
 *     4.4.7-4.4.9), computed here with a table of our own rather than
 *     miniz's mz_crc32;
 *   - the extraction paths agree: all succeed with the same bytes, or all
 *     refuse;
 *   - locating an entry by its own name finds an entry of that name.
 *
 * With ZU_FUZZ_ZIP_DUMP set in the environment it also prints what it
 * read, one line per archive and per entry, which tools/check-zip-reader.py
 * compares with Python's zipfile over the committed seed corpus. The fuzzer
 * never sets it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

/* The same bound the codec targets decode under (fuzz_common.h), for the
   same reason: an entry declaring 4 GiB is a bomb, not a finding. */
#define ZU_ZIP_MAX_OUTPUT  (16u * 1024u * 1024u)
#define ZU_ZIP_MAX_ENTRIES 512u
#define ZU_ZIP_READ_CHUNK  4096u

static volatile uint8_t zu_zip_sink;

/* Reads every delivered byte, for MSan: see zu_fuzz_consume(). */
static void zu_zip_consume(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (uint8_t) (acc ^ p[i]);
    }
    if (acc == 0xA5u) {
        zu_zip_sink++;
    }
}

/* CRC-32 as APPNOTE 4.4.7 defines it (the ISO 3309 polynomial, reflected),
   independent of miniz so that a miniz CRC bug cannot vouch for itself. */
static uint32_t zu_zip_crc_table[256];

static uint32_t zu_zip_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    if (zu_zip_crc_table[1] == 0) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            zu_zip_crc_table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc = zu_zip_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

static void zu_zip_fail(const char *what, mz_uint index)
{
    fprintf(stderr, "fuzz_zip_reader: oracle failed at entry %u: %s\n",
            (unsigned) index, what);
    abort();
}

static int zu_zip_dump = -1;

static void zu_zip_hex(const uint8_t *p, size_t n)
{
    if (n == 0) {
        fputc('-', stdout);
    }
    for (size_t i = 0; i < n; i++) {
        printf("%02x", p[i]);
    }
}

/* What one extraction path produced. */
typedef struct {
    int      tried;     /* 0 when the path was not attempted */
    int      bounded;   /* stopped at ZU_ZIP_MAX_OUTPUT, not at the end */
    int      ok;
    mz_zip_error err;
    uint8_t *data;
    size_t   len;
} zu_zip_result;

static void zu_zip_clear(mz_zip_archive *z)
{
    mz_zip_set_last_error(z, MZ_ZIP_NO_ERROR);
}

/* Through the iterator, to the end or to the output bound. */
static void zu_zip_via_iter(mz_zip_archive *z, mz_uint i, zu_zip_result *r)
{
    r->tried = 1;
    zu_zip_clear(z);
    mz_zip_reader_extract_iter_state *it =
        mz_zip_reader_extract_iter_new(z, i, 0);
    if (it == NULL) {
        r->err = mz_zip_get_last_error(z);
        if (r->err == MZ_ZIP_NO_ERROR) {
            zu_zip_fail("iter_new refused without an error code", i);
        }
        return;
    }
    size_t cap = ZU_ZIP_READ_CHUNK;
    r->data = (uint8_t *) malloc(cap);
    if (r->data == NULL) { abort(); }
    for (;;) {
        if (r->len + ZU_ZIP_READ_CHUNK > cap) {
            if (cap >= ZU_ZIP_MAX_OUTPUT) {
                r->bounded = 1;          /* as in fuzz_common.h */
                break;
            }
            cap *= 2;
            uint8_t *p = (uint8_t *) realloc(r->data, cap);
            if (p == NULL) { abort(); }
            r->data = p;
        }
        size_t got = mz_zip_reader_extract_iter_read(it, r->data + r->len,
                                                     ZU_ZIP_READ_CHUNK);
        if (got > ZU_ZIP_READ_CHUNK) {
            zu_zip_fail("iter_read reported more than it was given room for", i);
        }
        r->len += got;
        if (got == 0) {
            break;
        }
    }
    zu_zip_consume(r->data, r->len);
    r->ok = mz_zip_reader_extract_iter_free(it) ? 1 : 0;
    r->err = mz_zip_get_last_error(z);
}

/* Through a write callback, which appends; bounded like the iterator. */
typedef struct {
    zu_zip_result *r;
    size_t cap;
} zu_zip_sink_state;

static size_t zu_zip_write(void *opaque, mz_uint64 ofs, const void *p, size_t n)
{
    zu_zip_sink_state *k = (zu_zip_sink_state *) opaque;
    zu_zip_result *r = k->r;
    if (n == 0) {
        return 0;
    }
    if (ofs != r->len) {
        zu_zip_fail("the write callback was given a gap or an overlap", 0);
    }
    if (r->len + n > ZU_ZIP_MAX_OUTPUT) {
        r->bounded = 1;
        return 0;                        /* miniz: write callback failed */
    }
    if (r->len + n > k->cap) {
        size_t cap = k->cap ? k->cap : ZU_ZIP_READ_CHUNK;
        while (cap < r->len + n) { cap *= 2; }
        uint8_t *q = (uint8_t *) realloc(r->data, cap);
        if (q == NULL) { abort(); }
        r->data = q;
        k->cap = cap;
    }
    memcpy(r->data + r->len, p, n);
    r->len += n;
    return n;
}

static void zu_zip_via_callback(mz_zip_archive *z, mz_uint i, zu_zip_result *r)
{
    zu_zip_sink_state k;
    k.r = r;
    k.cap = 0;
    r->tried = 1;
    zu_zip_clear(z);
    r->ok = mz_zip_reader_extract_to_callback(z, i, zu_zip_write, &k, 0) ? 1 : 0;
    r->err = mz_zip_get_last_error(z);
    if (r->data != NULL) {
        zu_zip_consume(r->data, r->len);
    }
}

/* Into a buffer of exactly the declared size. */
static void zu_zip_via_mem(mz_zip_archive *z, mz_uint i,
                           const mz_zip_archive_file_stat *st,
                           zu_zip_result *r)
{
    if (st->m_uncomp_size > ZU_ZIP_MAX_OUTPUT) {
        return;                          /* not tried: past the bound */
    }
    r->tried = 1;
    r->len = (size_t) st->m_uncomp_size;
    r->data = (uint8_t *) malloc(r->len ? r->len : 1);
    if (r->data == NULL) { abort(); }
    /* Poisoned, so a success that writes nothing cannot pass for one that
       delivered bytes which happen to be zero. */
    memset(r->data, 0xA5, r->len);
    zu_zip_clear(z);
    r->ok = mz_zip_reader_extract_to_mem(z, i, r->data, r->len, 0) ? 1 : 0;
    r->err = mz_zip_get_last_error(z);
    zu_zip_consume(r->data, r->len);
}

static void zu_zip_check(mz_uint i, const mz_zip_archive_file_stat *st,
                         const zu_zip_result *r, const char *path)
{
    if (!r->tried || r->bounded) {
        return;          /* abandoned at the bound: nothing to judge */
    }
    if (!r->ok) {
        if (r->err == MZ_ZIP_NO_ERROR) {
            zu_zip_fail(path, i);
        }
        return;
    }
    if (st->m_is_directory) {
        return;          /* a directory's sizes say nothing about content */
    }
    if (r->len != st->m_uncomp_size) {
        zu_zip_fail("success delivered a size other than the declared one", i);
    }
    if (zu_zip_crc32(0, r->data, r->len) != st->m_crc32) {
        zu_zip_fail("success delivered bytes whose CRC-32 is not the entry's", i);
    }
}

/* Two paths that both ran to an answer must give the same one. */
static void zu_zip_agree(mz_uint i, const zu_zip_result *a,
                         const zu_zip_result *b, const char *which)
{
    if (!a->tried || !b->tried || a->bounded || b->bounded) {
        return;
    }
    if (a->ok != b->ok) {
        fprintf(stderr, "fuzz_zip_reader: %s disagree\n", which);
        zu_zip_fail("two extraction paths disagree on whether the entry "
                    "is readable", i);
    }
    if (a->ok && (a->len != b->len ||
                  (a->len && memcmp(a->data, b->data, a->len) != 0))) {
        fprintf(stderr, "fuzz_zip_reader: %s disagree\n", which);
        zu_zip_fail("two extraction paths delivered different bytes", i);
    }
}

static void zu_zip_print_result(const char *path, const zu_zip_result *r)
{
    if (!r->tried) {
        printf("\t%s=skipped", path);
    } else if (r->ok) {
        printf("\t%s=ok:", path);
        zu_zip_hex(r->data, r->len);
    } else {
        printf("\t%s=refused:%s", path, mz_zip_get_error_string(r->err));
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (zu_zip_dump < 0) {
        zu_zip_dump = getenv("ZU_FUZZ_ZIP_DUMP") != NULL;
    }

    mz_zip_archive z;
    memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_mem(&z, data, size, 0)) {
        mz_zip_error e = mz_zip_get_last_error(&z);
        if (e == MZ_ZIP_NO_ERROR) {
            zu_zip_fail("init_mem refused without an error code", 0);
        }
        if (zu_zip_dump) {
            printf("archive\trefused:%s\n", mz_zip_get_error_string(e));
        }
        return 0;
    }

    mz_uint n = mz_zip_reader_get_num_files(&z);
    if (zu_zip_dump) {
        printf("archive\tok\t%u\n", (unsigned) n);
    }
    if (n > ZU_ZIP_MAX_ENTRIES) {
        n = ZU_ZIP_MAX_ENTRIES;
    }

    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        zu_zip_clear(&z);
        if (!mz_zip_reader_file_stat(&z, i, &st)) {
            if (mz_zip_get_last_error(&z) == MZ_ZIP_NO_ERROR) {
                zu_zip_fail("file_stat refused without an error code", i);
            }
            if (zu_zip_dump) {
                printf("entry\t%u\tstat-refused:%s\n", (unsigned) i,
                       mz_zip_get_error_string(mz_zip_get_last_error(&z)));
            }
            continue;
        }

        /* The name the central directory gives, NUL-terminated by miniz
           within m_filename's bounds; locate must find that name. */
        size_t name_len = strlen(st.m_filename);
        mz_uint32 at = 0;
        zu_zip_clear(&z);
        if (name_len > 0 &&
            mz_zip_reader_locate_file_v2(&z, st.m_filename, NULL,
                                         MZ_ZIP_FLAG_CASE_SENSITIVE, &at)) {
            mz_zip_archive_file_stat other;
            if (!mz_zip_reader_file_stat(&z, at, &other) ||
                strcmp(other.m_filename, st.m_filename) != 0) {
                zu_zip_fail("locate found an entry of another name", i);
            }
        }

        zu_zip_result it, mem, cb;
        memset(&it, 0, sizeof(it));
        memset(&mem, 0, sizeof(mem));
        memset(&cb, 0, sizeof(cb));
        zu_zip_via_iter(&z, i, &it);
        zu_zip_via_mem(&z, i, &st, &mem);
        zu_zip_via_callback(&z, i, &cb);

        if (zu_zip_dump) {
            printf("entry\t%u\t", (unsigned) i);
            zu_zip_hex((const uint8_t *) st.m_filename, name_len);
            printf("\t%d\t%u\t%llu\t%llu\t%08x", st.m_is_directory ? 1 : 0,
                   (unsigned) st.m_method,
                   (unsigned long long) st.m_comp_size,
                   (unsigned long long) st.m_uncomp_size,
                   (unsigned) st.m_crc32);
            zu_zip_print_result("iter", &it);
            zu_zip_print_result("mem", &mem);
            zu_zip_print_result("callback", &cb);
            printf("\n");
        }

        zu_zip_check(i, &st, &it, "iter refused without an error code");
        zu_zip_check(i, &st, &mem, "extract_to_mem refused without an error code");
        zu_zip_check(i, &st, &cb, "extract_to_callback refused without an error code");
        if (!st.m_is_directory) {
            zu_zip_agree(i, &it, &mem, "the iterator and extract_to_mem");
            zu_zip_agree(i, &it, &cb, "the iterator and extract_to_callback");
            zu_zip_agree(i, &mem, &cb, "extract_to_mem and extract_to_callback");
        }
        free(it.data);
        free(mem.data);
        free(cb.data);
    }

    mz_zip_reader_end(&z);
    return 0;
}
