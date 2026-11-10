/* Entry point and shared helpers for zkt_ctest (#66); see zkt.h. */
#include "zkt.h"

long zkt_checks   = 0;
long zkt_failures = 0;

const zu_codec zkt_codecs[ZKT_N_CODECS] = {
    ZU_CODEC_IDENTITY, ZU_CODEC_DEFLATE_RAW, ZU_CODEC_ZLIB, ZU_CODEC_GZIP,
    ZU_CODEC_ZSTD, ZU_CODEC_LZ4_FRAME, ZU_CODEC_LZ4_BLOCK
};

void zkt_bytes_free(zkt_bytes *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static void zkt_reserve(zkt_bytes *b, size_t more)
{
    if (b->cap - b->n >= more) {
        return;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (cap - b->n < more) {
        cap *= 2;
    }
    uint8_t *p = realloc(b->p, cap);
    if (p == NULL) {
        fprintf(stderr, "zkt: out of memory\n");
        exit(2);
    }
    b->p = p;
    b->cap = cap;
}

void zkt_bytes_put(zkt_bytes *b, const uint8_t *p, size_t n)
{
    zkt_reserve(b, n);
    if (n > 0) {
        memcpy(b->p + b->n, p, n);
    }
    b->n += n;
}

int zkt_bytes_eq(const zkt_bytes *b, const uint8_t *p, size_t n)
{
    return b->n == n && (n == 0 || memcmp(b->p, p, n) == 0);
}

/* Words from a small vocabulary: long matches and short literals, so the
   DEFLATE, zstd and lz4 encoders all emit back-references. */
void zkt_payload_text(uint8_t *out, size_t n)
{
    static const char *const words[] = {
        "codec ", "registry ", "stream ", "the ", "of ", "limit ",
        "window ", "member ", "frame ", "block ", "zukomp ", "\n"
    };
    uint32_t s = 12345u;
    size_t i = 0;
    while (i < n) {
        s = s * 1103515245u + 12345u;
        const char *w = words[(s >> 16) % 12u];
        for (size_t k = 0; w[k] != '\0' && i < n; k++) {
            out[i++] = (uint8_t) w[k];
        }
    }
}

/* Incompressible: stored blocks for DEFLATE, raw literals elsewhere. */
void zkt_payload_lcg(uint8_t *out, size_t n)
{
    uint32_t s = 2463534242u;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        out[i] = (uint8_t) (s >> 24);
    }
}

zkt_drive_opts zkt_defaults(void)
{
    zkt_drive_opts o;
    memset(&o, 0, sizeof(o));
    o.in_chunk  = 4096;
    o.out_chunk = 4096;
    o.level     = ZU_LEVEL_DEFAULT;
    return o;
}

/* The loop is zu_int_run_whole()'s, minus R: refill only once the slice is
   spent, ZU_FINISH with the last slice, stop on STREAM_END or an error, and
   refuse to spin when nothing moves and there is nothing left to give. A
   stream that ends with input never offered applies the trailing policy to
   it, which the decoder cannot do for bytes it was not shown (#90). */
static zu_status zkt_drive(int encode, zu_encoder *e, zu_decoder *d,
                           const uint8_t *src, size_t n,
                           const zkt_drive_opts *o, zkt_bytes *out,
                           size_t *consumed)
{
    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));
    size_t   fed   = 0;
    unsigned calls = 0;
    zu_status st;

    for (;;) {
        if (buf.src_pos == buf.src_size && fed < n) {
            size_t take = n - fed;
            if (take > o->in_chunk) {
                take = o->in_chunk;
            }
            buf.src      = src + fed;
            buf.src_size = take;
            buf.src_pos  = 0;
            fed += take;
        }
        const int last = (fed >= n);
        zu_flush flush = last ? ZU_FINISH : ZU_RUN;
        if (!last && o->flush_every > 0 &&
            ((calls + 1) % o->flush_every) == 0) {
            flush = ZU_FLUSH;
        }

        zkt_reserve(out, o->out_chunk);
        buf.dst      = out->p + out->n;
        buf.dst_size = o->out_chunk;
        buf.dst_pos  = 0;

        st = encode ? zu_encoder_process(e, &buf, flush)
                    : zu_decoder_process(d, &buf, flush);
        out->n += buf.dst_pos;
        calls++;

        if (st == ZU_STREAM_END) {
            st = (!encode && fed < n && (o->flags & ZU_DEC_REJECT_TRAILING))
               ? ZU_ERR_TRAILING : ZU_OK;
            break;
        }
        if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
            break;
        }
        if (buf.dst_pos == 0 && last && st == ZU_NEED_INPUT) {
            st = ZU_ERR_INTERNAL;
            break;
        }
        if (calls > 50000000u) {
            st = ZU_ERR_INTERNAL;   /* a mutant that loops forever */
            break;
        }
    }
    if (consumed != NULL) {
        *consumed = fed - (buf.src_size - buf.src_pos);
    }
    return st;
}

zu_status zkt_encode(zu_codec codec, const uint8_t *src, size_t n,
                     const zkt_drive_opts *o, zkt_bytes *out)
{
    zu_encoder_opts eo;
    memset(&eo, 0, sizeof(eo));
    eo.struct_size = (uint32_t) sizeof(eo);
    eo.codec = codec;
    eo.level = o->level;
    zu_encoder *e = NULL;
    zu_status st = zu_encoder_new(&e, &eo);
    if (st != ZU_OK) {
        return st;
    }
    st = zkt_drive(1, e, NULL, src, n, o, out, NULL);
    zu_encoder_free(e);
    return st;
}

zu_status zkt_decode(zu_codec codec, const uint8_t *src, size_t n,
                     const zkt_drive_opts *o, zkt_bytes *out,
                     size_t *consumed)
{
    zu_decoder_opts dopt;
    memset(&dopt, 0, sizeof(dopt));
    dopt.struct_size   = (uint32_t) sizeof(dopt);
    dopt.codec         = codec;
    dopt.max_output    = o->max_output;
    dopt.max_ratio     = o->max_ratio;
    dopt.flags         = o->flags;
    dopt.expected_size = o->expected_size;
    zu_decoder *d = NULL;
    zu_status st = zu_decoder_new(&d, &dopt);
    if (st != ZU_OK) {
        return st;
    }
    st = zkt_drive(0, NULL, d, src, n, o, out, consumed);
    zu_decoder_free(d);
    return st;
}

void zkt_encode_ok(zu_codec codec, const uint8_t *src, size_t n,
                   zkt_bytes *out)
{
    zkt_drive_opts o = zkt_defaults();
    zu_status st = zkt_encode(codec, src, n, &o, out);
    if (st != ZU_OK) {
        fprintf(stderr, "zkt: encoding with codec %d failed: %s\n",
                (int) codec, zu_status_string(st));
        exit(2);
    }
}

int main(void)
{
    /* The registry is written once, before any stream exists -- the
       contract R_init_zukomp honours. A failure here leaves nothing to
       test. */
    zu_status st = zu_int_register_builtin_codecs();
    if (st != ZU_OK) {
        fprintf(stderr, "registering the built-in codecs failed: %s\n",
                zu_status_string(st));
        return 2;
    }

    zkt_suite_api();
    zkt_suite_roundtrip();
    zkt_suite_fixtures();
    zkt_suite_limits();
    zkt_suite_errors();
    zkt_suite_gzip();
    zkt_suite_buf();
    /* Last: it registers codecs, and the registry has no removal API. */
    zkt_suite_registry();

    printf("zkt_ctest: %ld checks, %ld failed\n", zkt_checks, zkt_failures);
    return zkt_failures == 0 ? 0 : 1;
}
