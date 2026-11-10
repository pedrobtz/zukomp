/* miniz patch 0002's history counter (#86).
 *
 * tools/patches/miniz/0002-validate-match-distance.patch bounds a match
 * distance by min(bytes emitted since tinfl_init(), window). The count is a
 * size_t, so on a 32-bit target it wrapped after 4 GiB through one stream,
 * and a valid match reaching back further than the wrapped count was then
 * rejected as corrupt. The count only ever meets a MZ_MIN against the
 * window, so it saturates there now.
 *
 * Two tests. One runs everywhere: it sets the counter next to SIZE_MAX part
 * way through a stream, which is the state 4 GiB of output leaves on 32-bit,
 * and requires the rest of the stream -- long matches included -- to decode.
 * The other is the real thing, more than 4 GiB through one decoder, and
 * runs only where size_t is narrower than 64 bits (native-checks' -m32
 * step); on 64-bit it would take minutes and could not wrap. */
#include "zkt.h"

#include "miniz.h"

/* The first member of miniz.c's private inflate_state is the tinfl
   decompressor, so a pointer to the state is a pointer to it. */
static tinfl_decompressor *zkt_tinfl(mz_stream *s)
{
    return (tinfl_decompressor *) s->state;
}

static void zkt_counter_near_overflow(void)
{
    /* Random bytes, then the same bytes again: the second half is matches
       at distance N, beyond anything a wrapped count would allow. */
    enum { N = 20000 };
    static uint8_t src[2 * N], out[2 * N];
    zkt_payload_lcg(src, N);
    memcpy(src + N, src, N);
    zkt_bytes enc = {0};
    zkt_encode_ok(ZU_CODEC_DEFLATE_RAW, src, sizeof(src), &enc);
    ZKT_CHECK(enc.n < N + N / 4);      /* the second half really is matches */

    mz_stream s;
    memset(&s, 0, sizeof(s));
    ZKT_CHECK(mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) == MZ_OK);
    s.next_in = enc.p;
    s.avail_in = (unsigned int) enc.n;
    size_t got = 0;
    int st = MZ_OK, tampered = 0;
    while (st == MZ_OK && got < sizeof(out)) {
        /* Small output windows: mz_inflate()'s wrapping path, where the
           patch's counter is what bounds the distance. */
        s.next_out = out + got;
        s.avail_out = 1000;
        st = mz_inflate(&s, MZ_SYNC_FLUSH);
        got = (size_t) (s.next_out - out);
        if (!tampered && got >= 5000) {
            /* Before any match: the counter as 4 GiB on 32-bit leaves it. */
            zkt_tinfl(&s)->m_total_out = SIZE_MAX - 3;
            tampered = 1;
        }
    }
    ZKT_CHECK(tampered);
    ZKT_CHECK(st == MZ_STREAM_END);
    ZKT_CHECK(got == sizeof(out) && memcmp(out, src, sizeof(out)) == 0);
    if (st != MZ_STREAM_END) {
        fprintf(stderr, "  near-overflow counter: mz_inflate %d after %zu bytes\n",
                st, got);
    }
    mz_inflateEnd(&s);
    zkt_bytes_free(&enc);
}

#if SIZE_MAX < UINT64_MAX
/* More than 4 GiB of a repeating random block through one encoder and one
   decoder, each fed the other's output as it appears, so nothing is stored. */
static void zkt_four_gib(void)
{
    enum { BLOCK = 20000, CHUNK = 1u << 16 };
    static uint8_t block[BLOCK], enc_out[CHUNK], dec_out[CHUNK];
    zkt_payload_lcg(block, BLOCK);
    const uint64_t total = (UINT64_C(1) << 32) + (UINT64_C(1) << 20);

    zu_encoder_opts eo;
    memset(&eo, 0, sizeof(eo));
    eo.struct_size = (uint32_t) sizeof(eo);
    eo.codec = ZU_CODEC_DEFLATE_RAW;
    eo.level = 1;
    zu_decoder_opts dopt;
    memset(&dopt, 0, sizeof(dopt));
    dopt.struct_size = (uint32_t) sizeof(dopt);
    dopt.codec = ZU_CODEC_DEFLATE_RAW;
    zu_encoder *e = NULL;
    zu_decoder *d = NULL;
    ZKT_STATUS(zu_encoder_new(&e, &eo), ZU_OK);
    ZKT_STATUS(zu_decoder_new(&d, &dopt), ZU_OK);

    uint64_t fed = 0, decoded = 0;
    int ok = 1, enc_done = 0;
    zu_status ds = ZU_OK;
    while (ok && ds != ZU_STREAM_END) {
        /* One block of input per round, the last one cut to size. */
        size_t take = BLOCK;
        if (total - fed < take) {
            take = (size_t) (total - fed);
        }
        zu_buffer eb;
        memset(&eb, 0, sizeof(eb));
        eb.src = block;
        eb.src_size = take;
        int last = (fed + take == total);
        do {
            eb.dst = enc_out;
            eb.dst_size = CHUNK;
            eb.dst_pos = 0;
            zu_status es = enc_done ? ZU_STREAM_END
                                    : zu_encoder_process(e, &eb, last ? ZU_FINISH : ZU_RUN);
            if (es == ZU_STREAM_END) {
                enc_done = 1;
            } else if (es != ZU_OK && es != ZU_NEED_INPUT && es != ZU_NEED_OUTPUT) {
                ok = 0;
                break;
            }
            /* Hand what the encoder wrote straight to the decoder. */
            zu_buffer db;
            memset(&db, 0, sizeof(db));
            db.src = enc_out;
            db.src_size = eb.dst_pos;
            while (db.src_pos < db.src_size || (enc_done && ds != ZU_STREAM_END)) {
                db.dst = dec_out;
                db.dst_size = CHUNK;
                db.dst_pos = 0;
                ds = zu_decoder_process(d, &db, enc_done ? ZU_FINISH : ZU_RUN);
                for (size_t i = 0; i < db.dst_pos; i++) {
                    if (dec_out[i] != block[(decoded + i) % BLOCK]) {
                        ok = 0;
                        break;
                    }
                }
                decoded += db.dst_pos;
                if (ds != ZU_OK && ds != ZU_NEED_INPUT && ds != ZU_NEED_OUTPUT &&
                    ds != ZU_STREAM_END) {
                    fprintf(stderr, "  4 GiB: decoder %s at %llu bytes\n",
                            zu_status_string(ds), (unsigned long long) decoded);
                    ok = 0;
                    break;
                }
                if (ds == ZU_STREAM_END || (db.dst_pos == 0 && db.src_pos == db.src_size)) {
                    break;
                }
            }
        } while (ok && eb.src_pos < eb.src_size);
        fed += take;
        if (enc_done && ds != ZU_STREAM_END) {
            ok = 0;
        }
    }
    ZKT_CHECK(ok);
    ZKT_CHECK(decoded == total);
    zu_encoder_free(e);
    zu_decoder_free(d);
}
#endif

void zkt_suite_miniz(void)
{
    zkt_counter_near_overflow();
#if SIZE_MAX < UINT64_MAX
    zkt_four_gib();
#endif
}
