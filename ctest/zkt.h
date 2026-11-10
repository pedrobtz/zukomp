/* The standalone C suite's scaffolding (#66).
 *
 * zkt_ctest links the pure-C core -- registry, driver, buffers, codecs, gzip
 * parser, frames -- and the vendored libraries, with no R in the process, the
 * way fuzz/build.sh does. It exists so that the core can be checked without
 * R, which is what mutation testing needs: Mull changes one operator in
 * src/ (one file at a time), reruns this binary, and a mutant counts as killed only if a check
 * here fails. So every check compares against something that knows the
 * answer -- the input of a round trip, a status design 7 names, a byte count
 * the specification fixes -- never "it did not crash". */
#ifndef ZKT_H
#define ZKT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zukomp.h"
#include "zu_internal.h"

extern long zkt_checks;
extern long zkt_failures;

/* Reports and counts; never aborts, so one run lists every failure. */
#define ZKT_CHECK(cond)                                                    \
    do {                                                                   \
        zkt_checks++;                                                      \
        if (!(cond)) {                                                     \
            zkt_failures++;                                                \
            fprintf(stderr, "%s:%d: FAILED %s\n", __FILE__, __LINE__,      \
                    #cond);                                                \
        }                                                                  \
    } while (0)

/* Status comparisons name both statuses, which is what makes a failure
   readable from the log alone. */
#define ZKT_STATUS(got, want)                                              \
    do {                                                                   \
        zu_status zkt_g_ = (got);                                          \
        zu_status zkt_w_ = (want);                                         \
        zkt_checks++;                                                      \
        if (zkt_g_ != zkt_w_) {                                            \
            zkt_failures++;                                                \
            fprintf(stderr, "%s:%d: FAILED %s is %s, want %s\n",           \
                    __FILE__, __LINE__, #got, zu_status_string(zkt_g_),    \
                    zu_status_string(zkt_w_));                             \
        }                                                                  \
    } while (0)

/* The seven codecs this build implements, in registration order. */
#define ZKT_N_CODECS 7
extern const zu_codec zkt_codecs[ZKT_N_CODECS];

/* A growable byte vector for test outputs. */
typedef struct {
    uint8_t *p;
    size_t   n;
    size_t   cap;
} zkt_bytes;

void zkt_bytes_free(zkt_bytes *b);
void zkt_bytes_put(zkt_bytes *b, const uint8_t *p, size_t n);
int  zkt_bytes_eq(const zkt_bytes *b, const uint8_t *p, size_t n);

/* Deterministic payloads. "text" is compressible, "lcg" is not. */
void zkt_payload_text(uint8_t *out, size_t n);
void zkt_payload_lcg(uint8_t *out, size_t n);

/* How drive() feeds a stream: at most in_chunk input and out_chunk output
   room per process() call, ZU_FINISH arriving with the last input slice
   (as zu_int_run_whole() does), and ZU_FLUSH every flush_every-th call. */
typedef struct {
    size_t   in_chunk;
    size_t   out_chunk;
    unsigned flush_every;          /* 0 = never */
    int32_t  level;                /* encoders */
    uint64_t max_output;           /* decoders */
    uint32_t max_ratio;
    uint32_t flags;
    uint64_t expected_size;
} zkt_drive_opts;

zkt_drive_opts zkt_defaults(void);

/* Encodes or decodes src through a fresh handle. Returns the terminal
   status (ZU_OK for a stream that ended), with the output in *out and the
   input bytes the codec took in *consumed (if non-NULL). */
zu_status zkt_encode(zu_codec codec, const uint8_t *src, size_t n,
                     const zkt_drive_opts *o, zkt_bytes *out);
zu_status zkt_decode(zu_codec codec, const uint8_t *src, size_t n,
                     const zkt_drive_opts *o, zkt_bytes *out,
                     size_t *consumed);

/* Encode with defaults; aborts the run if that fails, because every test
   after it would be testing nothing. */
void zkt_encode_ok(zu_codec codec, const uint8_t *src, size_t n,
                   zkt_bytes *out);

void zkt_suite_api(void);
void zkt_suite_registry(void);
void zkt_suite_roundtrip(void);
void zkt_suite_limits(void);
void zkt_suite_errors(void);
void zkt_suite_gzip(void);
void zkt_suite_buf(void);
void zkt_suite_fixtures(void);
void zkt_suite_miniz(void);

#endif /* ZKT_H */
