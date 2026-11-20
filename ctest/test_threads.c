/* The thread contract of zukomp_api_v1 (#133), with no R in the process.
 *
 * The body is tools/zukomptest/src/zkt_threads.h, the same file the table
 * fixture compiles. ctest cannot include zukomp-r.h, which needs R, so the
 * table here is a struct of the same members holding the same zu_*
 * functions src/zukomp_api.c puts in zukomp_api_v1 --
 * zukomp_test_api_table_identity() checks that every slot of the real table
 * is exactly one of these functions, so a result here stands for the table.
 *
 * ZKT_THREADS (default 2) and ZKT_THREAD_ROUNDS (default 1) size the run;
 * `make tsan` raises both. */
#define _POSIX_C_SOURCE 200809L
#include "zkt.h"

typedef struct {
    zu_codec  (*codec_lookup)(const char *name);
    zu_status (*encoder_new)(zu_encoder **out, const zu_encoder_opts *opts);
    zu_status (*encoder_process)(zu_encoder *e, zu_buffer *buf, zu_flush flush);
    zu_status (*encoder_reset)(zu_encoder *e, const zu_encoder_opts *opts);
    void      (*encoder_free)(zu_encoder *e);
    zu_status (*decoder_new)(zu_decoder **out, const zu_decoder_opts *opts);
    zu_status (*decoder_process)(zu_decoder *d, zu_buffer *buf, zu_flush flush);
    zu_status (*decoder_reset)(zu_decoder *d, const zu_decoder_opts *opts);
    void      (*decoder_free)(zu_decoder *d);
    zu_status (*compress_bound)(zu_codec codec, int32_t level, size_t n,
                                size_t *out);
    zu_status (*compress_one)(const zu_encoder_opts *opts,
                              const uint8_t *src, size_t n,
                              uint8_t *dst, size_t cap, size_t *written);
    zu_status (*decompress_one)(const zu_decoder_opts *opts,
                                const uint8_t *src, size_t n,
                                uint8_t *dst, size_t cap, size_t *written);
} zkt_table;

#define ZKT_API zkt_table
#include "zkt_threads.h"

static int zkt_env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') {
        return dflt;
    }
    return atoi(v);
}

void zkt_suite_threads(void)
{
    static const zkt_table table = {
        zu_codec_lookup,
        zu_encoder_new, zu_encoder_process, zu_encoder_reset, zu_encoder_free,
        zu_decoder_new, zu_decoder_process, zu_decoder_reset, zu_decoder_free,
        zu_compress_bound, zu_compress_one, zu_decompress_one
    };
    char msg[256] = "";
    long checks = 0;

    int rc = zkt_thr_page_caps(&table, &checks, msg, sizeof(msg));
    zkt_checks += checks;
    ZKT_CHECK(rc == 0);
    if (rc != 0) {
        fprintf(stderr, "  %s\n", msg);
    }

    const int threads = zkt_env_int("ZKT_THREADS", 2);
    const int rounds  = zkt_env_int("ZKT_THREAD_ROUNDS", 1);
    rc = zkt_thr_run(&table, threads, rounds, &checks, msg, sizeof(msg));
    zkt_checks += checks;
    ZKT_CHECK(rc == 0);
    if (rc != 0) {
        fprintf(stderr, "  %s\n", msg);
    }
    ZKT_CHECK(rc != 0 ||
              checks == (long) threads * rounds * ZKT_THR_N_JOBS * 4);
}
