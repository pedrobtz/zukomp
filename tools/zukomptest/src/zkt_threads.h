/* The thread contract of zukomp_api_v1, tested (#133).
 *
 * zukomp.h promises that every function reached through the table may run
 * on any thread, concurrently with the others, as long as each encoder or
 * decoder is used by one thread at a time. This file is the test of that
 * promise, written once and compiled twice (design 22, decision 33):
 *
 *   tools/zukomptest/src/threads.c   through the real table, fetched with
 *                                    zukomp_api() as a consumer such as
 *                                    zugodb fetches it, on Linux, macOS and
 *                                    Windows (tools/ci/consumer.sh)
 *   ctest/test_threads.c             through a table of the same zu_*
 *                                    functions, with no R in the process,
 *                                    under -Werror, ASan + UBSan, -m32 and
 *                                    ThreadSanitizer (make -C ctest tsan)
 *
 * The includer defines ZKT_API as a struct type with zukomp_api_v1's member
 * names (zukomptest uses zukomp_api_v1 itself; ctest, which cannot include
 * zukomp-r.h without R, a struct of the subset used here). Everything is
 * static, and nothing here calls R: the workers run off the main thread.
 *
 * What it checks. Setup runs on the calling thread: for zstd, lz4-frame and
 * lz4-block and several payload sizes it encodes each payload one-shot and
 * streaming, and decodes the one-shot stream, which must give the payload
 * back. Then n threads each loop over every job, in an order of their own,
 * and with states of their own: one decoder and one encoder per codec,
 * reused page after page through reset, as a page reader keeps one decoder
 * per worker (qio#17). Each decode -- streaming at two input chunk sizes,
 * and one-shot -- and each encode, streaming and one-shot, is compared byte
 * for byte with the single-threaded result. A worker writes only its own
 * report; the calling thread reads them after joining.
 *
 * ZKT_SHARE_STATES is the negative control: every worker then uses one set
 * of states created by the calling thread, which breaks the contract on
 * purpose. Only ctest's tsan-negative target builds it, and that target
 * passes only if ThreadSanitizer reports the race. Anywhere else it is
 * undefined behaviour that might crash or pass by luck. */
#ifndef ZKT_THREADS_H
#define ZKT_THREADS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zukomp.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#ifndef ZKT_API
#error "define ZKT_API as a struct with zukomp_api_v1's member names"
#endif

#define ZKT_THR_N_CODECS 3
#define ZKT_THR_N_SIZES  4
#define ZKT_THR_N_JOBS   (ZKT_THR_N_CODECS * ZKT_THR_N_SIZES)
#define ZKT_THR_MAX      64

static const char *const zkt_thr_codec_names[ZKT_THR_N_CODECS] = {
    "zstd", "lz4-frame", "lz4-block"
};

/* Across a 64 KiB lz4-frame block and zstd's 128 KiB block, so the larger
   ones take more than one block and more than one process() call. */
static const size_t zkt_thr_sizes[ZKT_THR_N_SIZES] = {
    1, 5000, 70001, 300007
};

typedef struct {
    zu_codec codec;
    const char *name;
    size_t   n;
    uint8_t *payload;
    uint8_t *enc_one;      /* zu_compress_one(), single-threaded */
    size_t   enc_one_n;
    uint8_t *enc_stream;   /* a fresh streaming encoder, single-threaded */
    size_t   enc_stream_n;
    uint8_t *dec;          /* enc_one decoded single-threaded */
    size_t   cap;          /* compress bound: the largest encoding */
} zkt_thr_job;

typedef struct {
    const ZKT_API     *api;
    const zkt_thr_job *jobs;
    int                index;
    int                rounds;
    zu_decoder       **shared_dec;   /* ZKT_SHARE_STATES only */
    zu_encoder       **shared_enc;
    long               checks;
    int                failed;
    char               msg[256];
} zkt_thr_worker;

static void zkt_thr_fail(char *msg, size_t cap, const char *what,
                         const char *codec, size_t n, int index,
                         zu_status st)
{
    snprintf(msg, cap, "thread %d, %s, %lu bytes: %s (status %d)",
             index, codec, (unsigned long) n, what, (int) st);
}

/* Compressible text with an incompressible stretch every few KiB, so every
   encoder emits matches and literals both. */
static void zkt_thr_payload(uint8_t *out, size_t n, uint32_t seed)
{
    static const char *const words[] = {
        "page ", "column ", "worker ", "decoder ", "the ", "of ",
        "frame ", "block ", "zukomp ", "thread ", "\n"
    };
    uint32_t s = seed * 2654435761u + 1u;
    size_t i = 0;
    while (i < n) {
        s = s * 1103515245u + 12345u;
        if ((s >> 8) % 64u == 0u) {
            for (size_t k = 0; k < 97 && i < n; k++) {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                out[i++] = (uint8_t) (s >> 24);
            }
            continue;
        }
        const char *w = words[(s >> 16) % 11u];
        for (size_t k = 0; w[k] != '\0' && i < n; k++) {
            out[i++] = (uint8_t) w[k];
        }
    }
}

static void zkt_thr_dec_opts(zu_decoder_opts *o, zu_codec codec, size_t n)
{
    memset(o, 0, sizeof(*o));
    o->struct_size   = (uint32_t) sizeof(*o);
    o->codec         = codec;
    o->flags         = ZU_DEC_REJECT_TRAILING | ZU_DEC_EXPECTED_SIZE;
    o->expected_size = (uint64_t) n;
}

static void zkt_thr_enc_opts(zu_encoder_opts *o, zu_codec codec)
{
    memset(o, 0, sizeof(*o));
    o->struct_size = (uint32_t) sizeof(*o);
    o->codec       = codec;
    o->level       = ZU_LEVEL_DEFAULT;
}

/* Feeds src in slices of `chunk` (ZU_FINISH with the last) to a decoder
   just reset for this page, into dst[0..cap). */
static zu_status zkt_thr_decode_stream(const ZKT_API *api, zu_decoder *d,
                                       const uint8_t *src, size_t n,
                                       size_t chunk, uint8_t *dst,
                                       size_t cap, size_t *written)
{
    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.dst = dst;
    buf.dst_size = cap;
    size_t fed = 0;
    unsigned long calls = 0;
    zu_status st;
    *written = 0;
    for (;;) {
        if (buf.src_pos == buf.src_size && fed < n) {
            size_t take = (n - fed < chunk) ? n - fed : chunk;
            buf.src = src + fed;
            buf.src_size = take;
            buf.src_pos = 0;
            fed += take;
        }
        const size_t in0 = buf.src_pos, out0 = buf.dst_pos;
        st = api->decoder_process(d, &buf, (fed >= n) ? ZU_FINISH : ZU_RUN);
        *written = buf.dst_pos;
        if (st == ZU_STREAM_END) {
            return ZU_OK;
        }
        if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
            return st;
        }
        if (st == ZU_NEED_OUTPUT && buf.dst_pos == buf.dst_size) {
            return ZU_NEED_OUTPUT;            /* the caller's buffer is full */
        }
        if (buf.src_pos == in0 && buf.dst_pos == out0 &&
            buf.src_pos == buf.src_size && fed >= n) {
            return ZU_ERR_INTERNAL;           /* stuck: refuse to spin */
        }
        if (++calls > 10000000ul) {
            return ZU_ERR_INTERNAL;
        }
    }
}

/* The whole payload at ZU_FINISH, into a buffer of the compress bound. */
static zu_status zkt_thr_encode_stream(const ZKT_API *api, zu_encoder *e,
                                       const uint8_t *src, size_t n,
                                       uint8_t *dst, size_t cap,
                                       size_t *written)
{
    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.src = src;
    buf.src_size = n;
    buf.dst = dst;
    buf.dst_size = cap;
    *written = 0;
    for (unsigned long calls = 0; calls < 10000000ul; calls++) {
        const size_t in0 = buf.src_pos, out0 = buf.dst_pos;
        zu_status st = api->encoder_process(e, &buf, ZU_FINISH);
        *written = buf.dst_pos;
        if (st == ZU_STREAM_END) {
            return (buf.src_pos == n) ? ZU_OK : ZU_ERR_INTERNAL;
        }
        if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
            return st;
        }
        if (buf.src_pos == in0 && buf.dst_pos == out0) {
            return ZU_ERR_INTERNAL;
        }
    }
    return ZU_ERR_INTERNAL;
}

static int zkt_thr_same(const uint8_t *a, size_t an,
                        const uint8_t *b, size_t bn)
{
    return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

static void zkt_thr_jobs_free(zkt_thr_job *jobs)
{
    for (int j = 0; j < ZKT_THR_N_JOBS; j++) {
        free(jobs[j].payload);
        free(jobs[j].enc_one);
        free(jobs[j].enc_stream);
        free(jobs[j].dec);
        memset(&jobs[j], 0, sizeof(jobs[j]));
    }
}

/* Single-threaded references, on the calling thread. */
static int zkt_thr_jobs_make(const ZKT_API *api, zkt_thr_job *jobs,
                             char *msg, size_t msg_cap)
{
    memset(jobs, 0, sizeof(*jobs) * ZKT_THR_N_JOBS);
    for (int c = 0; c < ZKT_THR_N_CODECS; c++) {
        const zu_codec codec = api->codec_lookup(zkt_thr_codec_names[c]);
        for (int s = 0; s < ZKT_THR_N_SIZES; s++) {
            zkt_thr_job *j = &jobs[c * ZKT_THR_N_SIZES + s];
            j->codec = codec;
            j->name  = zkt_thr_codec_names[c];
            j->n     = zkt_thr_sizes[s];
            zu_status st = api->compress_bound(codec, ZU_LEVEL_DEFAULT, j->n,
                                               &j->cap);
            if (st != ZU_OK) {
                zkt_thr_fail(msg, msg_cap, "compress_bound", j->name, j->n,
                             -1, st);
                return 1;
            }
            j->payload    = malloc(j->n);
            j->enc_one    = malloc(j->cap);
            j->enc_stream = malloc(j->cap);
            j->dec        = malloc(j->n);
            if (!j->payload || !j->enc_one || !j->enc_stream || !j->dec) {
                snprintf(msg, msg_cap, "out of memory");
                return 1;
            }
            zkt_thr_payload(j->payload, j->n, (uint32_t) (c * 31 + s));

            zu_encoder_opts eo;
            zkt_thr_enc_opts(&eo, codec);
            st = api->compress_one(&eo, j->payload, j->n, j->enc_one, j->cap,
                                   &j->enc_one_n);
            if (st != ZU_OK) {
                zkt_thr_fail(msg, msg_cap, "reference compress_one", j->name,
                             j->n, -1, st);
                return 1;
            }
            zu_encoder *e = NULL;
            st = api->encoder_new(&e, &eo);
            if (st == ZU_OK) {
                st = zkt_thr_encode_stream(api, e, j->payload, j->n,
                                           j->enc_stream, j->cap,
                                           &j->enc_stream_n);
            }
            api->encoder_free(e);
            if (st != ZU_OK) {
                zkt_thr_fail(msg, msg_cap, "reference streaming encode",
                             j->name, j->n, -1, st);
                return 1;
            }
            zu_decoder_opts dopt;
            zkt_thr_dec_opts(&dopt, codec, j->n);
            size_t w = 0;
            st = api->decompress_one(&dopt, j->enc_one, j->enc_one_n, j->dec,
                                     j->n, &w);
            if (st != ZU_OK || !zkt_thr_same(j->dec, w, j->payload, j->n)) {
                zkt_thr_fail(msg, msg_cap, "reference decode", j->name, j->n,
                             -1, st);
                return 1;
            }
        }
    }
    return 0;
}

static void zkt_thr_work(zkt_thr_worker *wk)
{
    const ZKT_API *api = wk->api;
    zu_decoder *own_dec[ZKT_THR_N_CODECS] = { NULL, NULL, NULL };
    zu_encoder *own_enc[ZKT_THR_N_CODECS] = { NULL, NULL, NULL };
    zu_decoder **dec = own_dec;
    zu_encoder **enc = own_enc;
#ifdef ZKT_SHARE_STATES
    dec = wk->shared_dec;
    enc = wk->shared_enc;
#endif
    uint8_t *out = NULL, *eout = NULL;
    size_t out_cap = 0, eout_cap = 0;
    for (int j = 0; j < ZKT_THR_N_JOBS; j++) {
        if (wk->jobs[j].n > out_cap)   { out_cap = wk->jobs[j].n; }
        if (wk->jobs[j].cap > eout_cap) { eout_cap = wk->jobs[j].cap; }
    }
    out  = malloc(out_cap);
    eout = malloc(eout_cap);
    if (out == NULL || eout == NULL) {
        wk->failed = 1;
        snprintf(wk->msg, sizeof(wk->msg), "thread %d: out of memory",
                 wk->index);
        goto done;
    }

    for (int r = 0; r < wk->rounds && !wk->failed; r++) {
        for (int k = 0; k < ZKT_THR_N_JOBS && !wk->failed; k++) {
            /* Each thread walks the jobs from its own starting point, so
               different codecs and sizes are in flight at once. */
            const int ji = (k + wk->index * 5 + r) % ZKT_THR_N_JOBS;
            const zkt_thr_job *j = &wk->jobs[ji];
            const int c = ji / ZKT_THR_N_SIZES;
            zu_status st;
            size_t w = 0;

            /* Decode, streaming: one decoder per codec for the thread's
               life, reset to this page's size, as a page reader does. */
            zu_decoder_opts dopt;
            zkt_thr_dec_opts(&dopt, j->codec, j->n);
            st = (dec[c] == NULL) ? api->decoder_new(&dec[c], &dopt)
                                  : api->decoder_reset(dec[c], &dopt);
            if (st != ZU_OK) {
                wk->failed = 1;
                zkt_thr_fail(wk->msg, sizeof(wk->msg), "decoder new/reset",
                             j->name, j->n, wk->index, st);
                break;
            }
            const size_t chunk = ((r + wk->index) % 2 == 0) ? j->enc_one_n
                                                            : 4093;
            memset(out, 0, j->n);
            st = zkt_thr_decode_stream(api, dec[c], j->enc_one, j->enc_one_n,
                                       chunk ? chunk : 1, out, j->n, &w);
            wk->checks++;
            if (st != ZU_OK || !zkt_thr_same(out, w, j->dec, j->n)) {
                wk->failed = 1;
                zkt_thr_fail(wk->msg, sizeof(wk->msg),
                             "streaming decode differs from the "
                             "single-threaded one", j->name, j->n, wk->index,
                             st);
                break;
            }

            /* Decode, one-shot. */
            memset(out, 0, j->n);
            st = api->decompress_one(&dopt, j->enc_one, j->enc_one_n, out,
                                     j->n, &w);
            wk->checks++;
            if (st != ZU_OK || !zkt_thr_same(out, w, j->dec, j->n)) {
                wk->failed = 1;
                zkt_thr_fail(wk->msg, sizeof(wk->msg),
                             "one-shot decode differs from the "
                             "single-threaded one", j->name, j->n, wk->index,
                             st);
                break;
            }

            /* Encode, streaming, through a reset encoder of the thread's
               own; and one-shot. Both are deterministic, so each must equal
               its single-threaded reference byte for byte. */
            zu_encoder_opts eo;
            zkt_thr_enc_opts(&eo, j->codec);
            st = (enc[c] == NULL) ? api->encoder_new(&enc[c], &eo)
                                  : api->encoder_reset(enc[c], &eo);
            if (st == ZU_OK) {
                st = zkt_thr_encode_stream(api, enc[c], j->payload, j->n,
                                           eout, j->cap, &w);
            }
            wk->checks++;
            if (st != ZU_OK ||
                !zkt_thr_same(eout, w, j->enc_stream, j->enc_stream_n)) {
                wk->failed = 1;
                zkt_thr_fail(wk->msg, sizeof(wk->msg),
                             "streaming encode differs from the "
                             "single-threaded one", j->name, j->n, wk->index,
                             st);
                break;
            }
            st = api->compress_one(&eo, j->payload, j->n, eout, j->cap, &w);
            wk->checks++;
            if (st != ZU_OK ||
                !zkt_thr_same(eout, w, j->enc_one, j->enc_one_n)) {
                wk->failed = 1;
                zkt_thr_fail(wk->msg, sizeof(wk->msg),
                             "one-shot encode differs from the "
                             "single-threaded one", j->name, j->n, wk->index,
                             st);
                break;
            }
        }
    }

done:
    free(out);
    free(eout);
    if (dec == own_dec) {
        for (int c = 0; c < ZKT_THR_N_CODECS; c++) {
            api->decoder_free(own_dec[c]);
            api->encoder_free(own_enc[c]);
        }
    }
}

#ifdef _WIN32
typedef HANDLE zkt_thr_handle;
static DWORD WINAPI zkt_thr_entry(LPVOID p)
{
    zkt_thr_work((zkt_thr_worker *) p);
    return 0;
}
static int zkt_thr_start(zkt_thr_handle *h, zkt_thr_worker *wk)
{
    *h = CreateThread(NULL, 0, zkt_thr_entry, wk, 0, NULL);
    return *h != NULL;
}
static void zkt_thr_join(zkt_thr_handle h)
{
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}
#else
typedef pthread_t zkt_thr_handle;
static void *zkt_thr_entry(void *p)
{
    zkt_thr_work((zkt_thr_worker *) p);
    return NULL;
}
static int zkt_thr_start(zkt_thr_handle *h, zkt_thr_worker *wk)
{
    return pthread_create(h, NULL, zkt_thr_entry, wk) == 0;
}
static void zkt_thr_join(zkt_thr_handle h)
{
    pthread_join(h, NULL);
}
#endif

/* Runs n_threads workers for `rounds` passes over every job. Returns 0 when
   every output matched, and otherwise 1 with the first failure in msg.
   *checks_out counts the comparisons made. */
static int zkt_thr_run(const ZKT_API *api, int n_threads, int rounds,
                       long *checks_out, char *msg, size_t msg_cap)
{
    zkt_thr_job *jobs = calloc(ZKT_THR_N_JOBS, sizeof(*jobs));
    zkt_thr_worker wk[ZKT_THR_MAX];
    zkt_thr_handle th[ZKT_THR_MAX];
    zu_decoder *shared_dec[ZKT_THR_N_CODECS] = { NULL, NULL, NULL };
    zu_encoder *shared_enc[ZKT_THR_N_CODECS] = { NULL, NULL, NULL };
    int started = 0, rc = 0;

    *checks_out = 0;
    if (n_threads < 1 || n_threads > ZKT_THR_MAX || rounds < 1) {
        snprintf(msg, msg_cap, "threads must be in 1..%d, rounds >= 1",
                 ZKT_THR_MAX);
        free(jobs);
        return 1;
    }
    if (jobs == NULL) {
        snprintf(msg, msg_cap, "out of memory");
        return 1;
    }
    if (zkt_thr_jobs_make(api, jobs, msg, msg_cap) != 0) {
        zkt_thr_jobs_free(jobs);
        free(jobs);
        return 1;
    }
#ifdef ZKT_SHARE_STATES
    /* The contract broken on purpose: one set of states for every thread. */
    for (int c = 0; c < ZKT_THR_N_CODECS; c++) {
        zu_decoder_opts dopt;
        zu_encoder_opts eo;
        zkt_thr_dec_opts(&dopt, jobs[c * ZKT_THR_N_SIZES].codec, 1);
        zkt_thr_enc_opts(&eo, jobs[c * ZKT_THR_N_SIZES].codec);
        (void) api->decoder_new(&shared_dec[c], &dopt);
        (void) api->encoder_new(&shared_enc[c], &eo);
    }
#endif
    for (int i = 0; i < n_threads; i++) {
        memset(&wk[i], 0, sizeof(wk[i]));
        wk[i].api        = api;
        wk[i].jobs       = jobs;
        wk[i].index      = i;
        wk[i].rounds     = rounds;
        wk[i].shared_dec = shared_dec;
        wk[i].shared_enc = shared_enc;
    }
    for (; started < n_threads; started++) {
        if (!zkt_thr_start(&th[started], &wk[started])) {
            snprintf(msg, msg_cap, "could not start thread %d", started);
            rc = 1;
            break;
        }
    }
    for (int i = 0; i < started; i++) {
        zkt_thr_join(th[i]);
    }
    for (int i = 0; i < started; i++) {
        *checks_out += wk[i].checks;
        if (rc == 0 && wk[i].failed) {
            snprintf(msg, msg_cap, "%s", wk[i].msg);
            rc = 1;
        }
    }
    for (int c = 0; c < ZKT_THR_N_CODECS; c++) {
        api->decoder_free(shared_dec[c]);
        api->encoder_free(shared_enc[c]);
    }
    zkt_thr_jobs_free(jobs);
    free(jobs);
    return rc;
}

/* A Parquet page, decoded the two ways a page reader can (qio#17): the
   decompressed size n comes from the page header and is passed as
   ZU_DEC_EXPECTED_SIZE, and the sink holds cap bytes, at cap = n - 1, n and
   n + 1. One-shot, and through one reused decoder. Below n the call fails
   -- ZU_ERR_OUTPUT_LIMIT one-shot, ZU_NEED_OUTPUT with the sink full from a
   decoder -- and at n or above it gives exactly the n bytes. Either way not
   a byte is written past min(n, cap): the guard after the sink, and the
   slack inside it, keep their fill. Returns 0, or 1 with msg. */
#define ZKT_PAGE_GUARD 32
static int zkt_thr_page_caps(const ZKT_API *api, long *checks_out,
                             char *msg, size_t msg_cap)
{
    static const char *const names[2] = { "zstd", "lz4-block" };
    static const size_t sizes[3] = { 1, 4096, 70001 };
    int rc = 0;
    *checks_out = 0;
    for (int c = 0; c < 2 && rc == 0; c++) {
        const zu_codec codec = api->codec_lookup(names[c]);
        zu_decoder *d = NULL;
        for (int s = 0; s < 3 && rc == 0; s++) {
            const size_t n = sizes[s];
            size_t bound = 0, en = 0;
            uint8_t *payload = malloc(n), *enc = NULL;
            uint8_t *sink = malloc(n + 1 + ZKT_PAGE_GUARD);
            zu_status st = api->compress_bound(codec, ZU_LEVEL_DEFAULT, n,
                                               &bound);
            if (st == ZU_OK) {
                enc = malloc(bound);
            }
            if (payload == NULL || sink == NULL || enc == NULL) {
                snprintf(msg, msg_cap, "page caps: setup failed");
                rc = 1;
            } else {
                zkt_thr_payload(payload, n, (uint32_t) (7 + s));
                zu_encoder_opts eo;
                zkt_thr_enc_opts(&eo, codec);
                st = api->compress_one(&eo, payload, n, enc, bound, &en);
                if (st != ZU_OK) {
                    zkt_thr_fail(msg, msg_cap, "page caps: compress",
                                 names[c], n, -1, st);
                    rc = 1;
                }
            }
            for (int way = 0; way < 2 && rc == 0; way++) {
                for (size_t cap = n - 1; cap <= n + 1 && rc == 0; cap++) {
                    zu_decoder_opts dopt;
                    zkt_thr_dec_opts(&dopt, codec, n);
                    memset(sink, 0xA5, n + 1 + ZKT_PAGE_GUARD);
                    size_t w = 0;
                    if (way == 0) {
                        st = api->decompress_one(&dopt, enc, en,
                                                 cap ? sink : NULL, cap, &w);
                    } else {
                        st = (d == NULL) ? api->decoder_new(&d, &dopt)
                                         : api->decoder_reset(d, &dopt);
                        if (st == ZU_OK) {
                            st = zkt_thr_decode_stream(api, d, enc, en, en,
                                                       cap ? sink : NULL,
                                                       cap, &w);
                        }
                    }
                    const zu_status want = (cap < n)
                        ? (way == 0 ? ZU_ERR_OUTPUT_LIMIT : ZU_NEED_OUTPUT)
                        : ZU_OK;
                    const size_t limit = (cap < n) ? cap : n;
                    int untouched = 1;
                    for (size_t i = limit; i < n + 1 + ZKT_PAGE_GUARD; i++) {
                        if (sink[i] != 0xA5) {
                            untouched = 0;
                            break;
                        }
                    }
                    const int body_ok = (cap < n)
                        ? (w <= cap && memcmp(sink, payload, w) == 0)
                        : zkt_thr_same(sink, w, payload, n);
                    *checks_out += 1;
                    if (st != want || !untouched || !body_ok) {
                        char what[96];
                        snprintf(what, sizeof(what),
                                 "%s at cap %lu: %s%s",
                                 way == 0 ? "decompress_one" : "decoder",
                                 (unsigned long) cap,
                                 st != want ? "wrong status" : "",
                                 !untouched ? " wrote past the cap"
                                 : (!body_ok ? " wrong bytes" : ""));
                        zkt_thr_fail(msg, msg_cap, what, names[c], n, -1, st);
                        rc = 1;
                    }
                }
            }
            free(payload);
            free(enc);
            free(sink);
        }
        api->decoder_free(d);
    }
    return rc;
}

#endif /* ZKT_THREADS_H */
