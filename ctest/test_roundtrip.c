/* Round trips: every codec, every chunking, every advertised level, flush.
 *
 * The oracle is the input. A round trip through one implementation cannot
 * see a symmetric mistake, which is what test_fixtures.c's external streams
 * are for; what it does see is any asymmetry, any boundary the chunking
 * moves, and any byte a flush fails to deliver. */
#include "zkt.h"

#include "miniz.h"

static void zkt_roundtrip(zu_codec codec, const uint8_t *src, size_t n,
                          const zkt_drive_opts *eo, const zkt_drive_opts *dopt,
                          const char *what)
{
    zkt_bytes enc = {0}, dec = {0};
    zu_status st = zkt_encode(codec, src, n, eo, &enc);
    ZKT_STATUS(st, ZU_OK);
    if (st == ZU_OK) {
        size_t consumed = 0;
        st = zkt_decode(codec, enc.p, enc.n, dopt, &dec, &consumed);
        ZKT_STATUS(st, ZU_OK);
        ZKT_CHECK(zkt_bytes_eq(&dec, src, n));
        ZKT_CHECK(consumed == enc.n);
        if (st != ZU_OK || !zkt_bytes_eq(&dec, src, n)) {
            fprintf(stderr, "  codec %d, %s, n %zu, in %zu/%zu out %zu/%zu\n",
                    (int) codec, what, n, eo->in_chunk, dopt->in_chunk,
                    eo->out_chunk, dopt->out_chunk);
        }
    }
    zkt_bytes_free(&enc);
    zkt_bytes_free(&dec);
}

static void zkt_chunk_sweep(void)
{
    enum { BIG = 70000, SMALL = 1500 };
    static uint8_t text[BIG], lcg[BIG];
    zkt_payload_text(text, BIG);
    zkt_payload_lcg(lcg, BIG);

    static const size_t sizes[] = { 0, 1, 2, 31, 4096, 4097, SMALL };
    static const size_t chunks[][2] = { {1, 1}, {2, 3}, {7, 31}, {4096, 4096} };

    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            for (size_t k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
                zkt_drive_opts o = zkt_defaults();
                o.in_chunk  = chunks[k][0];
                o.out_chunk = chunks[k][1];
                zkt_drive_opts d = o;
                /* Decode at the other chunking, so encoder and decoder
                   boundaries never coincide. */
                d.in_chunk  = chunks[k][1];
                d.out_chunk = chunks[k][0];
                zkt_roundtrip(zkt_codecs[c], text, sizes[s], &o, &d, "text");
                zkt_roundtrip(zkt_codecs[c], lcg, sizes[s], &o, &d, "lcg");
            }
        }
        /* Past miniz's 32 KiB window, LZ4's 64 KiB block and zstd's 128 KiB
           block boundary, at sizes that are not a power of two. */
        zkt_drive_opts o = zkt_defaults();
        zkt_drive_opts d = o;
        d.in_chunk = 1000;
        d.out_chunk = 333;
        zkt_roundtrip(zkt_codecs[c], text, BIG, &o, &d, "text big");
        zkt_roundtrip(zkt_codecs[c], lcg, BIG, &o, &d, "lcg big");
    }
}

/* Compression has to compress. A mutant that makes an encoder emit stored
   blocks, or skip the match finder, still round-trips; this is what it
   fails. Identity must be exact. */
static void zkt_compresses(void)
{
    enum { N = 65536 };
    static uint8_t text[N], lcg[N];
    zkt_payload_text(text, N);
    zkt_payload_lcg(lcg, N);
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zkt_bytes enc = {0}, raw = {0};
        zkt_encode_ok(zkt_codecs[c], text, N, &enc);
        zkt_encode_ok(zkt_codecs[c], lcg, N, &raw);
        if (zkt_codecs[c] == ZU_CODEC_IDENTITY) {
            ZKT_CHECK(zkt_bytes_eq(&enc, text, N));
        } else {
            ZKT_CHECK(enc.n < N / 2);
            if (enc.n >= N / 2) {
                fprintf(stderr, "  codec %d: %zu of %d\n", (int) zkt_codecs[c], enc.n, N);
            }
            /* Incompressible input costs at most a small framing overhead,
               and never less than the input. */
            ZKT_CHECK(raw.n >= N);
            ZKT_CHECK(raw.n < N + N / 64 + 64);
        }
        size_t bound = 0;
        ZKT_STATUS(zu_compress_bound(zkt_codecs[c], ZU_LEVEL_DEFAULT, N, &bound),
                   ZU_OK);
        ZKT_CHECK(bound >= raw.n);
        ZKT_CHECK(bound >= enc.n);
        zkt_bytes_free(&enc);
        zkt_bytes_free(&raw);
    }
}

/* The wrappers' fixed fields, which RFC 1950 and 1952 and the frame formats
   pin: an encoder that writes a wrong header still round-trips through its
   own decoder. */
static void zkt_wrapper_bytes(void)
{
    enum { N = 3000 };
    static uint8_t text[N];
    zkt_payload_text(text, N);

    zkt_bytes z = {0};
    zkt_encode_ok(ZU_CODEC_ZLIB, text, N, &z);
    ZKT_CHECK(z.n > 6);
    if (z.n > 6) {
        /* CMF 0x78: deflate, 32 KiB window; FCHECK makes it a multiple of
           31; the trailer is the Adler-32 of the input, big-endian. */
        ZKT_CHECK(z.p[0] == 0x78);
        ZKT_CHECK(((z.p[0] << 8) | z.p[1]) % 31 == 0);
        ZKT_CHECK((z.p[1] & 0x20) == 0);             /* no preset dict */
        mz_ulong a = mz_adler32(MZ_ADLER32_INIT, text, N);
        const uint8_t *t = z.p + z.n - 4;
        ZKT_CHECK((((mz_ulong) t[0] << 24) | ((mz_ulong) t[1] << 16) |
                   ((mz_ulong) t[2] << 8) | t[3]) == a);
    }

    zkt_bytes g = {0};
    zkt_encode_ok(ZU_CODEC_GZIP, text, N, &g);
    ZKT_CHECK(g.n > 18);
    if (g.n > 18) {
        /* The deterministic header CLAUDE.md promises: mtime 0, OS 255, no
           optional fields; the trailer is CRC-32 then ISIZE, both LE. */
        static const uint8_t hdr[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff };
        ZKT_CHECK(memcmp(g.p, hdr, 4) == 0);
        ZKT_CHECK(memcmp(g.p + 4, hdr + 4, 4) == 0);
        ZKT_CHECK(g.p[9] == 0xff);
        mz_ulong crc = mz_crc32(MZ_CRC32_INIT, text, N);
        const uint8_t *t = g.p + g.n - 8;
        ZKT_CHECK(((mz_ulong) t[0] | ((mz_ulong) t[1] << 8) |
                   ((mz_ulong) t[2] << 16) | ((mz_ulong) t[3] << 24)) == crc);
        ZKT_CHECK(((size_t) t[4] | ((size_t) t[5] << 8) |
                   ((size_t) t[6] << 16) | ((size_t) t[7] << 24)) == N);

        /* The raw DEFLATE body between them is deflate-raw's output. */
        zkt_drive_opts o = zkt_defaults();
        zkt_bytes body = {0};
        ZKT_STATUS(zkt_decode(ZU_CODEC_DEFLATE_RAW, g.p + 10, g.n - 18, &o,
                              &body, NULL), ZU_OK);
        ZKT_CHECK(zkt_bytes_eq(&body, text, N));
        zkt_bytes_free(&body);
    }

    zkt_bytes s = {0};
    zkt_encode_ok(ZU_CODEC_ZSTD, text, N, &s);
    static const uint8_t zstd_magic[4] = { 0x28, 0xb5, 0x2f, 0xfd };
    ZKT_CHECK(s.n > 4 && memcmp(s.p, zstd_magic, 4) == 0);

    zkt_bytes l = {0};
    zkt_encode_ok(ZU_CODEC_LZ4_FRAME, text, N, &l);
    static const uint8_t lz4_magic[4] = { 0x04, 0x22, 0x4d, 0x18 };
    ZKT_CHECK(l.n > 4 && memcmp(l.p, lz4_magic, 4) == 0);

    /* Each encoder's own output is detected as its codec; deflate-raw and
       lz4-block, which have no magic, are never detected at all. */
    zu_codec seen;
    ZKT_STATUS(zu_sniff(z.p, z.n, &seen), ZU_OK);
    ZKT_CHECK(seen == ZU_CODEC_ZLIB);
    ZKT_STATUS(zu_sniff(g.p, g.n, &seen), ZU_OK);
    ZKT_CHECK(seen == ZU_CODEC_GZIP);
    ZKT_STATUS(zu_sniff(s.p, s.n, &seen), ZU_OK);
    ZKT_CHECK(seen == ZU_CODEC_ZSTD);
    ZKT_STATUS(zu_sniff(l.p, l.n, &seen), ZU_OK);
    ZKT_CHECK(seen == ZU_CODEC_LZ4_FRAME);

    zkt_bytes_free(&z);
    zkt_bytes_free(&g);
    zkt_bytes_free(&s);
    zkt_bytes_free(&l);
}

/* Every advertised level, and the abstract ones, round-trips; out of range
   is refused before a stream exists. Higher DEFLATE and zstd levels must
   not compress worse than level 1 on text, and level 0 is stored. */
static void zkt_levels(void)
{
    /* Small: zstd's level 22 and 19 and LZ4's HC 12 are slow, and this
       binary runs once per mutant. */
    enum { N = 3000 };
    static uint8_t text[N];
    zkt_payload_text(text, N);
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zu_codec codec = zkt_codecs[c];
        zu_codec_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t) sizeof(info);
        ZKT_STATUS(zu_codec_get_info(codec, &info), ZU_OK);
        int32_t levels[5] = { info.level_min, info.level_max,
                              info.level_default, info.level_fast,
                              info.level_best };
        int has_levels = !(info.level_min == 0 && info.level_max == 0 &&
                           info.level_default == 0);
        for (size_t k = 0; k < 5; k++) {
            zkt_drive_opts o = zkt_defaults();
            o.level = levels[k];
            zkt_drive_opts d = zkt_defaults();
            zkt_roundtrip(codec, text, N, &o, &d, "level");
        }
        zkt_bytes tmp = {0};
        zkt_drive_opts o = zkt_defaults();
        if (has_levels) {
            o.level = info.level_max + 1;
            ZKT_STATUS(zkt_encode(codec, text, 10, &o, &tmp), ZU_ERR_INVALID_ARGUMENT);
            o.level = info.level_min - 1;
            ZKT_STATUS(zkt_encode(codec, text, 10, &o, &tmp), ZU_ERR_INVALID_ARGUMENT);
            size_t b;
            ZKT_STATUS(zu_compress_bound(codec, info.level_max + 1, 10, &b),
                       ZU_ERR_INVALID_ARGUMENT);
        } else {
            o.level = 1;
            ZKT_STATUS(zkt_encode(codec, text, 10, &o, &tmp), ZU_ERR_INVALID_ARGUMENT);
            o.level = 0;
            ZKT_STATUS(zkt_encode(codec, text, 10, &o, &tmp), ZU_OK);
        }
        zkt_bytes_free(&tmp);
    }

    /* The level reaches the encoder: stored blocks at 0, real compression
       at 1, and 9 no worse than 1. */
    for (zu_codec codec = ZU_CODEC_DEFLATE_RAW; codec <= ZU_CODEC_GZIP;
         codec = (zu_codec) (codec + 1)) {
        size_t sz[3];
        int32_t lv[3] = { 0, 1, 9 };
        for (size_t k = 0; k < 3; k++) {
            zkt_drive_opts o = zkt_defaults();
            o.level = lv[k];
            zkt_bytes e = {0};
            ZKT_STATUS(zkt_encode(codec, text, N, &o, &e), ZU_OK);
            sz[k] = e.n;
            zkt_bytes_free(&e);
        }
        ZKT_CHECK(sz[0] >= N);
        ZKT_CHECK(sz[1] < N / 2);
        ZKT_CHECK(sz[2] <= sz[1]);
    }
    for (zu_codec codec = ZU_CODEC_ZSTD; codec <= ZU_CODEC_LZ4_BLOCK;
         codec = (zu_codec) (codec + 1)) {
        zu_codec_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t) sizeof(info);
        zu_codec_get_info(codec, &info);
        zkt_drive_opts fast = zkt_defaults(), best = zkt_defaults();
        fast.level = info.level_fast;
        best.level = info.level_best;
        zkt_bytes a = {0}, b = {0};
        ZKT_STATUS(zkt_encode(codec, text, N, &fast, &a), ZU_OK);
        ZKT_STATUS(zkt_encode(codec, text, N, &best, &b), ZU_OK);
        ZKT_CHECK(b.n <= a.n);
        zkt_bytes_free(&a);
        zkt_bytes_free(&b);
    }
}

/* ZU_FLUSH puts every byte so far on the wire: what the encoder has emitted
   after a flush decodes, with no ZU_FINISH, to exactly the input it has
   taken. That is the property a streaming request body needs, and a round
   trip cannot see it -- the bytes would still arrive at the finish. */
static void zkt_flush(void)
{
    enum { N = 9000, CUT = 5000 };
    static uint8_t text[N];
    zkt_payload_text(text, N);
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zu_codec codec = zkt_codecs[c];
        zu_codec_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t) sizeof(info);
        zu_codec_get_info(codec, &info);

        zu_encoder_opts eo;
        memset(&eo, 0, sizeof(eo));
        eo.struct_size = (uint32_t) sizeof(eo);
        eo.codec = codec;
        eo.level = ZU_LEVEL_DEFAULT;
        zu_encoder *e = NULL;
        ZKT_STATUS(zu_encoder_new(&e, &eo), ZU_OK);
        if (e == NULL) {
            continue;
        }
        static uint8_t out[2 * N + 1024];
        zu_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.src = text;
        buf.src_size = CUT;
        buf.dst = out;
        buf.dst_size = sizeof(out);
        zu_status st = zu_encoder_process(e, &buf, ZU_FLUSH);
        if (!(info.flags & ZU_CAN_FLUSH)) {
            ZKT_STATUS(st, ZU_ERR_UNSUPPORTED);
            ZKT_CHECK(buf.src_pos == 0 && buf.dst_pos == 0);
            zu_encoder_free(e);
            continue;
        }
        /* The flush is finished when the call returns: all input taken,
           output room left, and a status that says no more room is
           wanted. Its size is the data's, not the window's (#91). */
        int done = (st == ZU_OK || st == ZU_NEED_INPUT);
        ZKT_CHECK(done);
        ZKT_CHECK(buf.src_pos == CUT);
        ZKT_CHECK(buf.dst_pos < (codec == ZU_CODEC_IDENTITY ? CUT + 1 : CUT / 2));
        if (!done || buf.dst_pos >= (codec == ZU_CODEC_IDENTITY ? CUT + 1 : CUT / 2)) {
            fprintf(stderr, "  flush codec %d: %s, %zu in, %zu out\n", (int) codec,
                    zu_status_string(st), buf.src_pos, buf.dst_pos);
        }
        /* A second flush with nothing new adds at most one empty block. */
        size_t after_one = buf.dst_pos;
        st = zu_encoder_process(e, &buf, ZU_FLUSH);
        ZKT_CHECK(st == ZU_OK || st == ZU_NEED_INPUT);
        ZKT_CHECK(buf.dst_pos - after_one <= 8);

        /* Decode what is on the wire so far, without finishing. */
        zu_decoder_opts dopt;
        memset(&dopt, 0, sizeof(dopt));
        dopt.struct_size = (uint32_t) sizeof(dopt);
        dopt.codec = codec;
        zu_decoder *d = NULL;
        ZKT_STATUS(zu_decoder_new(&d, &dopt), ZU_OK);
        static uint8_t got[N + 16];
        zu_buffer db;
        memset(&db, 0, sizeof(db));
        db.src = out;
        db.src_size = buf.dst_pos;
        db.dst = got;
        db.dst_size = sizeof(got);
        for (int i = 0; i < 64 && db.src_pos < db.src_size; i++) {
            st = zu_decoder_process(d, &db, ZU_RUN);
            if (st != ZU_OK && st != ZU_NEED_INPUT) {
                break;
            }
        }
        ZKT_CHECK(db.dst_pos == CUT);
        ZKT_CHECK(memcmp(got, text, CUT) == 0);

        /* And the finished stream is the whole input. */
        buf.src_size = N;
        for (int i = 0; i < 64; i++) {
            st = zu_encoder_process(e, &buf, ZU_FINISH);
            if (st != ZU_OK && st != ZU_NEED_OUTPUT) {
                break;
            }
        }
        ZKT_STATUS(st, ZU_STREAM_END);
        db.src_size = buf.dst_pos;
        for (int i = 0; i < 64; i++) {
            st = zu_decoder_process(d, &db, ZU_FINISH);
            if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
                break;
            }
        }
        ZKT_STATUS(st, ZU_STREAM_END);
        ZKT_CHECK(db.dst_pos == N && memcmp(got, text, N) == 0);
        zu_decoder_free(d);
        zu_encoder_free(e);
    }

    /* Repeated flushes through the driver, at small chunks. */
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        if (zkt_codecs[c] == ZU_CODEC_LZ4_BLOCK) {
            continue;
        }
        zkt_drive_opts o = zkt_defaults();
        o.in_chunk = 97;
        o.out_chunk = 61;
        o.flush_every = 3;
        zkt_drive_opts d = zkt_defaults();
        zkt_roundtrip(zkt_codecs[c], text, N, &o, &d, "flush_every 3");
    }
}

/* The one-shot functions share the driver, so they inherit its limits: a
   destination one byte short is ZU_ERR_OUTPUT_LIMIT, exact fits. */
static void zkt_one_shot(void)
{
    enum { N = 5000 };
    static uint8_t text[N], enc[2 * N], dec[N + 1];
    zkt_payload_text(text, N);
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zu_codec codec = zkt_codecs[c];
        zu_encoder_opts eo;
        memset(&eo, 0, sizeof(eo));
        eo.struct_size = (uint32_t) sizeof(eo);
        eo.codec = codec;
        eo.level = ZU_LEVEL_DEFAULT;
        size_t en = 0, dn = 0;
        ZKT_STATUS(zu_compress_one(&eo, text, N, enc, sizeof(enc), &en), ZU_OK);
        ZKT_CHECK(en > 0);

        zu_decoder_opts dopt;
        memset(&dopt, 0, sizeof(dopt));
        dopt.struct_size = (uint32_t) sizeof(dopt);
        dopt.codec = codec;
        ZKT_STATUS(zu_decompress_one(&dopt, enc, en, dec, N, &dn), ZU_OK);
        ZKT_CHECK(dn == N && memcmp(dec, text, N) == 0);
        ZKT_STATUS(zu_decompress_one(&dopt, enc, en, dec, N - 1, &dn),
                   ZU_ERR_OUTPUT_LIMIT);
        ZKT_CHECK(dn <= N - 1);

        size_t short_n = 0;
        ZKT_STATUS(zu_compress_one(&eo, text, N, enc, en - 1, &short_n),
                   ZU_ERR_OUTPUT_LIMIT);
        ZKT_CHECK(short_n <= en - 1);
    }
}

/* A decoder that answers ZU_NEED_INPUT has no output left to give (design
   8: "progress possible with more input"). Having taken every byte it was
   offered, a decoder can still hold decoded output -- miniz up to its
   32 KiB dictionary, libzstd and liblz4 their internal buffers -- and
   every streaming decoder answered ZU_NEED_INPUT there once the caller's
   window had filled, so a caller that waited for input stalled with that
   output owed. The input arrives in pieces of k bytes, the way a network
   delivers it; after every ZU_NEED_INPUT, a call offering no input must
   produce nothing. */
static void zkt_owed_output(void)
{
    enum { N = 1 << 17, ROOM = 4096 };
    static uint8_t zeros[N], text[N];
    zkt_payload_text(text, N);
    const uint8_t *payloads[2] = { zeros, text };
    static const zu_codec codecs[5] = {
        ZU_CODEC_DEFLATE_RAW, ZU_CODEC_ZLIB, ZU_CODEC_GZIP,
        ZU_CODEC_ZSTD, ZU_CODEC_LZ4_FRAME
    };
    static const size_t pieces[3] = { 1, 7, 64 };
    static uint8_t window[ROOM];

    for (size_t p = 0; p < 2; p++) {
        for (size_t c = 0; c < 5; c++) {
            zkt_bytes z;
            memset(&z, 0, sizeof(z));
            zkt_encode_ok(codecs[c], payloads[p], N, &z);
            for (size_t ki = 0; ki < 3; ki++) {
                zu_decoder_opts o;
                memset(&o, 0, sizeof(o));
                o.struct_size = (uint32_t) sizeof(o);
                o.codec = codecs[c];
                o.flags = ZU_DEC_CONCAT_MEMBERS | ZU_DEC_REJECT_TRAILING;
                zu_decoder *d = NULL;
                ZKT_STATUS(zu_decoder_new(&d, &o), ZU_OK);
                if (d == NULL) {
                    continue;
                }
                zkt_bytes out;
                memset(&out, 0, sizeof(out));
                size_t pos = 0, owed = 0, calls = 0;
                zu_status st = ZU_OK;
                while (calls++ < 4u * (z.n + N)) {
                    const size_t left = z.n - pos;
                    const size_t piece = left < pieces[ki] ? left : pieces[ki];
                    /* ZU_FINISH once the whole input has been taken. */
                    const int last = (left == 0);
                    zu_buffer buf;
                    memset(&buf, 0, sizeof(buf));
                    buf.src = piece ? z.p + pos : NULL;
                    buf.src_size = piece;
                    buf.dst = window;
                    buf.dst_size = ROOM;
                    st = zu_decoder_process(d, &buf, last ? ZU_FINISH : ZU_RUN);
                    pos += buf.src_pos;
                    zkt_bytes_put(&out, window, buf.dst_pos);
                    if (st == ZU_STREAM_END || (st != ZU_NEED_INPUT &&
                                                st != ZU_NEED_OUTPUT &&
                                                st != ZU_OK)) {
                        break;
                    }
                    if (st == ZU_NEED_INPUT) {
                        zu_buffer empty;
                        memset(&empty, 0, sizeof(empty));
                        empty.dst = window;
                        empty.dst_size = ROOM;
                        (void) zu_decoder_process(d, &empty, ZU_RUN);
                        if (empty.dst_pos != 0) {
                            owed++;
                            zkt_bytes_put(&out, window, empty.dst_pos);
                        }
                    }
                }
                ZKT_CHECK(owed == 0);
                ZKT_STATUS(st, ZU_STREAM_END);
                ZKT_CHECK(zkt_bytes_eq(&out, payloads[p], N));
                if (owed != 0 || st != ZU_STREAM_END) {
                    fprintf(stderr, "  codec %d, payload %zu, k = %zu: %zu "
                            "NEED_INPUT answers with output owed\n",
                            (int) codecs[c], p, pieces[ki], owed);
                }
                zkt_bytes_free(&out);
                zu_decoder_free(d);
            }
            zkt_bytes_free(&z);
        }
    }
}

/* Two edges of the stall probe's owed byte (codec_deflate.c), from review:
 *
 *   A reset discards it. A decoder holding an owed byte that is reset and
 *   given a new stream must not deliver the old stream's byte first.
 *
 *   A body cut short exactly at max_output is invalid data, the class it
 *   has with no limit (design 7: a short body is ZU_ERR_INVALID_DATA). With
 *   the window at the cap and nothing left to read, the probe finds nothing
 *   pending, so there is no output for the limit to refuse; before the
 *   probe this was reported as ZU_ERR_OUTPUT_LIMIT. */
static void zkt_owed_edges(void)
{
    /* A: deflate-raw of 1000 zeros, offered whole with no output room. */
    {
        static uint8_t zeros[1000];
        zkt_bytes z, h;
        memset(&z, 0, sizeof(z));
        memset(&h, 0, sizeof(h));
        zkt_encode_ok(ZU_CODEC_DEFLATE_RAW, zeros, sizeof(zeros), &z);
        static const uint8_t hello[5] = { 'h', 'e', 'l', 'l', 'o' };
        zkt_encode_ok(ZU_CODEC_DEFLATE_RAW, hello, sizeof(hello), &h);

        zu_decoder_opts o;
        memset(&o, 0, sizeof(o));
        o.struct_size = (uint32_t) sizeof(o);
        o.codec = ZU_CODEC_DEFLATE_RAW;
        zu_decoder *d = NULL;
        ZKT_STATUS(zu_decoder_new(&d, &o), ZU_OK);
        if (d != NULL) {
            zu_buffer b;
            memset(&b, 0, sizeof(b));
            b.src = z.p;
            b.src_size = z.n;
            /* No room: the probe takes a byte and owes it. */
            ZKT_STATUS(zu_decoder_process(d, &b, ZU_RUN), ZU_NEED_OUTPUT);
            ZKT_STATUS(zu_decoder_reset(d, &o), ZU_OK);

            static uint8_t got[64];
            memset(&b, 0, sizeof(b));
            b.src = h.p;
            b.src_size = h.n;
            b.dst = got;
            b.dst_size = sizeof(got);
            ZKT_STATUS(zu_decoder_process(d, &b, ZU_FINISH), ZU_STREAM_END);
            ZKT_CHECK(b.dst_pos == sizeof(hello) &&
                      memcmp(got, hello, sizeof(hello)) == 0);
            zu_decoder_free(d);
        }
        zkt_bytes_free(&z);
        zkt_bytes_free(&h);
    }

    /* B: a short body at exactly the cap, for each DEFLATE codec and a
       spread of cut points inside the body. */
    {
        enum { N = 5000 };
        static uint8_t text[N];
        zkt_payload_text(text, N);
        static const zu_codec codecs[3] = {
            ZU_CODEC_DEFLATE_RAW, ZU_CODEC_ZLIB, ZU_CODEC_GZIP
        };
        for (size_t c = 0; c < 3; c++) {
            zkt_bytes z;
            memset(&z, 0, sizeof(z));
            zkt_encode_ok(codecs[c], text, N, &z);
            /* Inside the body: past any header (gzip's is 10 bytes), well
               before the trailer. */
            for (size_t cut = 20; cut + 16 < z.n; cut += (z.n - 36) / 8 + 1) {
                zkt_drive_opts op = zkt_defaults();
                zkt_bytes free_run;
                memset(&free_run, 0, sizeof(free_run));
                zu_status st = zkt_decode(codecs[c], z.p, cut, &op, &free_run,
                                          NULL);
                ZKT_STATUS(st, ZU_ERR_INVALID_DATA);
                if (free_run.n == 0) {
                    zkt_bytes_free(&free_run);
                    continue;   /* max_output = 0 would mean no limit */
                }
                op.max_output = free_run.n;
                zkt_bytes capped;
                memset(&capped, 0, sizeof(capped));
                st = zkt_decode(codecs[c], z.p, cut, &op, &capped, NULL);
                ZKT_STATUS(st, ZU_ERR_INVALID_DATA);
                ZKT_CHECK(capped.n == free_run.n);
                if (st != ZU_ERR_INVALID_DATA) {
                    fprintf(stderr, "  codec %d cut at %zu of %zu, cap %zu\n",
                            (int) codecs[c], cut, z.n, free_run.n);
                }
                zkt_bytes_free(&free_run);
                zkt_bytes_free(&capped);
            }
            zkt_bytes_free(&z);
        }
    }
}

void zkt_suite_roundtrip(void)
{
    zkt_chunk_sweep();
    zkt_compresses();
    zkt_wrapper_bytes();
    zkt_levels();
    zkt_flush();
    zkt_one_shot();
    zkt_owed_output();
    zkt_owed_edges();
}
