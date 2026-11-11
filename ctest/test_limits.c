/* Limits, and what a decoder does with input that is wrong.
 *
 * The limits live in zu_decoder_process(), not in a codec (CLAUDE.md, "Limits
 * live in the core driver"), so every codec is driven through them. The
 * oracle for a limit is its boundary: one byte under fails with the status
 * design 7 names, the boundary itself and one over succeed with the whole
 * payload. A mutant that moves the boundary by one fails here, which is the
 * check issue #66 asks for of `remaining < allowance`.
 *
 * The DEFLATE family rejects a valid stream that ends exactly at the limit
 * when an input chunk ends between the last output byte and the end of the
 * stream (#64). None of the chunkings here does that; the full in_chunk sweep
 * is #65's. */
#include "zkt.h"

/* Output windows that divide the payload, with the whole input visible:
   the window then ends exactly on the cap, which is where "finished at the
   cap" and "would have exceeded it" have to be told apart. */
static const size_t zkt_limit_chunks[][2] = {
    {1u << 20, 1u << 20}, {1, 1}, {7, 3}, {500, 1000}, {64, 3000},
    {1u << 20, 3000}, {1u << 20, 1500}, {1u << 20, 1000}, {1u << 20, 1}
};
#define ZKT_N_LIMIT_CHUNKS (sizeof(zkt_limit_chunks) / sizeof(zkt_limit_chunks[0]))

static zu_status zkt_decode_at(zu_codec codec, const zkt_bytes *enc, size_t k,
                               zkt_drive_opts o, zkt_bytes *got)
{
    o.in_chunk  = zkt_limit_chunks[k][0];
    o.out_chunk = zkt_limit_chunks[k][1];
    got->n = 0;
    return zkt_decode(codec, enc->p, enc->n, &o, got, NULL);
}

static void zkt_max_output(void)
{
    enum { N = 3000 };
    static uint8_t text[N];
    zkt_payload_text(text, N);
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zu_codec codec = zkt_codecs[c];
        zkt_bytes enc = {0}, got = {0};
        zkt_encode_ok(codec, text, N, &enc);
        for (size_t k = 0; k < ZKT_N_LIMIT_CHUNKS; k++) {
            zkt_drive_opts o = zkt_defaults();

            o.max_output = N - 1;
            ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_ERR_OUTPUT_LIMIT);
            /* The cap is a window, not a post-hoc check: not one byte past
               it ever reaches the caller's buffer. */
            ZKT_CHECK(got.n <= N - 1);
            ZKT_CHECK(memcmp(got.p, text, got.n) == 0);

            o.max_output = 1;
            ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_ERR_OUTPUT_LIMIT);
            ZKT_CHECK(got.n <= 1);

            {
                o.max_output = N;
                ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_OK);
                ZKT_CHECK(zkt_bytes_eq(&got, text, N));
                if (!zkt_bytes_eq(&got, text, N)) {
                    fprintf(stderr, "  codec %d at max_output %d, chunks %zu/%zu\n",
                            (int) codec, N, zkt_limit_chunks[k][0],
                            zkt_limit_chunks[k][1]);
                }
            }

            o.max_output = N + 1;
            ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_OK);
            ZKT_CHECK(zkt_bytes_eq(&got, text, N));
        }
        zkt_bytes_free(&enc);
        zkt_bytes_free(&got);
    }
}

static void zkt_expected_size(void)
{
    enum { N = 3000 };
    static uint8_t text[N];
    zkt_payload_text(text, N);
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zu_codec codec = zkt_codecs[c];
        zkt_bytes enc = {0}, got = {0};
        zkt_encode_ok(codec, text, N, &enc);
        for (size_t k = 0; k < ZKT_N_LIMIT_CHUNKS; k++) {
            zkt_drive_opts o = zkt_defaults();
            o.flags = ZU_DEC_EXPECTED_SIZE;

            /* Too small: the data disagrees with the caller, which is
               invalid data, not a policy refusal. */
            o.expected_size = N - 1;
            ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_ERR_INVALID_DATA);
            ZKT_CHECK(got.n <= N - 1);

            /* Too large: the stream ends short of what was promised. */
            o.expected_size = N + 1;
            ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_ERR_INVALID_DATA);

            {
                o.expected_size = N;
                ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_OK);
                ZKT_CHECK(zkt_bytes_eq(&got, text, N));

                /* A tighter max_output still binds, as a policy refusal. */
                o.max_output = N - 1;
                ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_ERR_OUTPUT_LIMIT);
                /* A looser one does not relax the expected size. */
                o.max_output = N + 10;
                o.expected_size = N - 1;
                ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_ERR_INVALID_DATA);
                /* On a tie the expected size is the explanation. */
                o.max_output = N - 1;
                ZKT_STATUS(zkt_decode_at(codec, &enc, k, o, &got), ZU_ERR_INVALID_DATA);
            }
        }
        /* Zero is a real size: an empty stream decodes under it, and any
           byte at all is too many. */
        zkt_bytes empty = {0};
        zkt_encode_ok(codec, text, 0, &empty);
        zkt_drive_opts o = zkt_defaults();
        o.flags = ZU_DEC_EXPECTED_SIZE;
        o.expected_size = 0;
        got.n = 0;
        ZKT_STATUS(zkt_decode(codec, empty.p, empty.n, &o, &got, NULL), ZU_OK);
        ZKT_CHECK(got.n == 0);
        got.n = 0;
        ZKT_STATUS(zkt_decode(codec, enc.p, enc.n, &o, &got, NULL), ZU_ERR_INVALID_DATA);
        ZKT_CHECK(got.n == 0);
        zkt_bytes_free(&empty);
        zkt_bytes_free(&enc);
        zkt_bytes_free(&got);
    }
}

/* max_ratio bounds decoded bytes per input byte. Whole-buffer, so the check
   sees the whole input (#80 is how it depends on the window otherwise). */
static void zkt_max_ratio(void)
{
    enum { N = 20000 };
    static uint8_t zeros[N];
    memset(zeros, 0, sizeof(zeros));
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zu_codec codec = zkt_codecs[c];
        zkt_bytes enc = {0}, got = {0};
        zkt_encode_ok(codec, zeros, N, &enc);
        zkt_drive_opts o = zkt_defaults();
        o.in_chunk = o.out_chunk = 1u << 20;
        uint32_t ratio = (uint32_t) (N / enc.n);   /* floor: N > ratio * n */
        if (codec == ZU_CODEC_IDENTITY) {
            o.max_ratio = 1;
            ZKT_STATUS(zkt_decode(codec, enc.p, enc.n, &o, &got, NULL), ZU_OK);
            ZKT_CHECK(zkt_bytes_eq(&got, zeros, N));
        } else {
            ZKT_CHECK(ratio >= 2);
            o.max_ratio = ratio;
            ZKT_STATUS(zkt_decode(codec, enc.p, enc.n, &o, &got, NULL),
                       N % enc.n == 0 ? ZU_OK : ZU_ERR_RATIO_LIMIT);
            got.n = 0;
            o.max_ratio = ratio + 1;
            ZKT_STATUS(zkt_decode(codec, enc.p, enc.n, &o, &got, NULL), ZU_OK);
            ZKT_CHECK(zkt_bytes_eq(&got, zeros, N));
            got.n = 0;
            o.max_ratio = 1;
            ZKT_STATUS(zkt_decode(codec, enc.p, enc.n, &o, &got, NULL),
                       ZU_ERR_RATIO_LIMIT);
        }
        /* An enormous ratio cannot overflow into a refusal. */
        got.n = 0;
        o.max_ratio = UINT32_MAX;
        ZKT_STATUS(zkt_decode(codec, enc.p, enc.n, &o, &got, NULL), ZU_OK);
        zkt_bytes_free(&enc);
        zkt_bytes_free(&got);
    }
}

/* Bytes after the stream are policy: the codec stops exactly at the end,
   and ZU_DEC_REJECT_TRAILING decides. */
static void zkt_trailing(void)
{
    enum { N = 2000 };
    static uint8_t text[N];
    zkt_payload_text(text, N);
    static const uint8_t junk[5] = { 'j', 'u', 'n', 'k', '!' };
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zu_codec codec = zkt_codecs[c];
        if (codec == ZU_CODEC_IDENTITY || codec == ZU_CODEC_LZ4_BLOCK) {
            continue;   /* no end of stream of their own */
        }
        zkt_bytes enc = {0}, got = {0};
        zkt_encode_ok(codec, text, N, &enc);
        size_t stream_n = enc.n;
        zkt_bytes_put(&enc, junk, sizeof(junk));
        static const size_t chunks[] = { 1u << 20, 1, 3 };
        for (size_t k = 0; k < 3; k++) {
            zkt_drive_opts o = zkt_defaults();
            o.in_chunk = o.out_chunk = chunks[k];
            o.flags = ZU_DEC_REJECT_TRAILING | ZU_DEC_CONCAT_MEMBERS;
            got.n = 0;
            size_t used = 0;
            zu_status tst = zkt_decode(codec, enc.p, enc.n, &o, &got, &used);
            ZKT_STATUS(tst, ZU_ERR_TRAILING);
            ZKT_CHECK(zkt_bytes_eq(&got, text, N));
            if (tst != ZU_ERR_TRAILING) {
                fprintf(stderr, "  codec %d chunk %zu: consumed %zu of %zu (stream %zu)\n",
                        (int) codec, chunks[k], used, enc.n, stream_n);
            }

            o.flags = ZU_DEC_CONCAT_MEMBERS;
            got.n = 0;
            size_t consumed = 0;
            ZKT_STATUS(zkt_decode(codec, enc.p, enc.n, &o, &got, &consumed), ZU_OK);
            ZKT_CHECK(zkt_bytes_eq(&got, text, N));
            ZKT_CHECK(consumed == stream_n);
        }
        zkt_bytes_free(&enc);
        zkt_bytes_free(&got);
    }
}

/* Two members, gzip's or zstd's or lz4's frames, with and without a
   skippable frame between them: ZU_DEC_CONCAT_MEMBERS continues, and
   without it the first stream is the whole answer. */
static void zkt_members(void)
{
    enum { N = 1500 };
    static uint8_t a[N], b[N];
    zkt_payload_text(a, N);
    zkt_payload_lcg(b, N);
    static const zu_codec framed[] = { ZU_CODEC_GZIP, ZU_CODEC_ZSTD, ZU_CODEC_LZ4_FRAME };
    for (size_t c = 0; c < 3; c++) {
        zu_codec codec = framed[c];
        for (int skippable = 0; skippable < 2; skippable++) {
            if (skippable && codec == ZU_CODEC_GZIP) {
                continue;
            }
            zkt_bytes two = {0}, ea = {0}, eb = {0}, want = {0}, got = {0};
            zkt_encode_ok(codec, a, N, &ea);
            zkt_encode_ok(codec, b, N, &eb);
            zkt_bytes_put(&two, ea.p, ea.n);
            if (skippable) {
                /* Magic 0x184D2A5? little-endian, then a 4-byte LE size:
                   the same skippable frame in RFC 8878 and LZ4's format. */
                static const uint8_t skip[] = { 0x53, 0x2a, 0x4d, 0x18, 3, 0, 0, 0,
                                                0xaa, 0xbb, 0xcc };
                zkt_bytes_put(&two, skip, sizeof(skip));
            }
            zkt_bytes_put(&two, eb.p, eb.n);
            zkt_bytes_put(&want, a, N);
            zkt_bytes_put(&want, b, N);

            static const size_t chunks[] = { 1u << 20, 1, 2, 5 };
            for (size_t k = 0; k < 4; k++) {
                zkt_drive_opts o = zkt_defaults();
                o.in_chunk = o.out_chunk = chunks[k];
                o.flags = ZU_DEC_REJECT_TRAILING | ZU_DEC_CONCAT_MEMBERS;
                got.n = 0;
                size_t consumed = 0;
                ZKT_STATUS(zkt_decode(codec, two.p, two.n, &o, &got, &consumed), ZU_OK);
                ZKT_CHECK(zkt_bytes_eq(&got, want.p, want.n));
                ZKT_CHECK(consumed == two.n);

                o.flags = 0;
                got.n = 0;
                ZKT_STATUS(zkt_decode(codec, two.p, two.n, &o, &got, &consumed), ZU_OK);
                ZKT_CHECK(zkt_bytes_eq(&got, a, N));
                ZKT_CHECK(consumed == ea.n);

                o.flags = ZU_DEC_REJECT_TRAILING;
                got.n = 0;
                zu_status st = zkt_decode(codec, two.p, two.n, &o, &got, &consumed);
                ZKT_STATUS(st, ZU_ERR_TRAILING);
                if (st != ZU_ERR_TRAILING) {
                    fprintf(stderr, "  codec %d skippable %d chunk %zu: got %zu bytes consumed %zu of %zu (first %zu)\n",
                            (int) codec, skippable, chunks[k], got.n, consumed, two.n, ea.n);
                }
            }
            zkt_bytes_free(&two);
            zkt_bytes_free(&ea);
            zkt_bytes_free(&eb);
            zkt_bytes_free(&want);
            zkt_bytes_free(&got);
        }
    }
}

/* The core's accounting when a call does not start at the beginning of
   either buffer (#66: mutants in zu_int_decoder_step() survived because
   every other check here calls with src_pos = dst_pos = 0, where
   "position - start" and "position + start" agree). A caller that keeps
   one input buffer across calls, or appends output to a buffer it already
   wrote, starts mid-buffer; the limit window, the totals the limits read
   and the identity copy must all count from where the call starts. */
static void zkt_entry_offsets(void)
{
    /* PRE_IN as long as the stream, so that an accounting that counted the
       caller's offset as consumed input would double it. */
    enum { N = 5000, PRE_IN = 4096, PRE_OUT = 100 };
    static uint8_t text[N];
    zkt_payload_text(text, N);
    zkt_bytes z;
    memset(&z, 0, sizeof(z));
    zkt_encode_ok(ZU_CODEC_DEFLATE_RAW, text, N, &z);

    /* PRE_IN junk bytes, then the stream; PRE_OUT bytes already written. */
    static uint8_t in[PRE_IN + 65536];
    memset(in, 0xEE, PRE_IN);
    memcpy(in + PRE_IN, z.p, z.n);
    static uint8_t out[PRE_OUT + 2 * N];

    zu_decoder_opts o;
    memset(&o, 0, sizeof(o));
    o.struct_size = (uint32_t) sizeof(o);
    o.codec = ZU_CODEC_DEFLATE_RAW;

    /* max_output below the stream: the call stops at exactly the cap,
       counted from where it started, and says so on that call. */
    for (uint64_t cap = 1000; cap <= 3000; cap += 1000) {
        o.max_output = cap;
        zu_decoder *d = NULL;
        ZKT_STATUS(zu_decoder_new(&d, &o), ZU_OK);
        if (d == NULL) { continue; }
        zu_buffer b;
        memset(&b, 0, sizeof(b));
        b.src = in;
        b.src_size = PRE_IN + z.n;
        b.src_pos = PRE_IN;
        b.dst = out;
        b.dst_size = sizeof(out);
        b.dst_pos = PRE_OUT;
        ZKT_STATUS(zu_decoder_process(d, &b, ZU_FINISH), ZU_ERR_OUTPUT_LIMIT);
        ZKT_CHECK(b.dst_pos - PRE_OUT == cap);
        ZKT_CHECK(memcmp(out + PRE_OUT, text, (size_t) cap) == 0);
        zu_decoder_free(d);
    }

    /* max_output exactly the stream: it ends, it is not over the limit. */
    o.max_output = N;
    {
        zu_decoder *d = NULL;
        ZKT_STATUS(zu_decoder_new(&d, &o), ZU_OK);
        if (d != NULL) {
            zu_buffer b;
            memset(&b, 0, sizeof(b));
            b.src = in;
            b.src_size = PRE_IN + z.n;
            b.src_pos = PRE_IN;
            b.dst = out;
            b.dst_size = sizeof(out);
            b.dst_pos = PRE_OUT;
            ZKT_STATUS(zu_decoder_process(d, &b, ZU_FINISH), ZU_STREAM_END);
            ZKT_CHECK(b.src_pos == PRE_IN + z.n);
            ZKT_CHECK(b.dst_pos == PRE_OUT + N);
            ZKT_CHECK(memcmp(out + PRE_OUT, text, N) == 0);
            zu_decoder_free(d);
        }
    }

    /* max_ratio: the ratio is output over the input this stream consumed,
       not over the input's position in the caller's buffer. */
    o.max_output = 0;
    {
        const double ratio = (double) N / (double) z.n;
        o.max_ratio = (uint32_t) ratio;        /* just below: must refuse */
        zu_decoder *d = NULL;
        ZKT_STATUS(zu_decoder_new(&d, &o), ZU_OK);
        if (d != NULL) {
            zu_buffer b;
            memset(&b, 0, sizeof(b));
            b.src = in;
            b.src_size = PRE_IN + z.n;
            b.src_pos = PRE_IN;
            b.dst = out;
            b.dst_size = sizeof(out);
            b.dst_pos = PRE_OUT;
            ZKT_STATUS(zu_decoder_process(d, &b, ZU_FINISH), ZU_ERR_RATIO_LIMIT);
            zu_decoder_free(d);
        }
        o.max_ratio = (uint32_t) ratio + 1;    /* above: must decode */
        d = NULL;
        ZKT_STATUS(zu_decoder_new(&d, &o), ZU_OK);
        if (d != NULL) {
            zu_buffer b;
            memset(&b, 0, sizeof(b));
            b.src = in;
            b.src_size = PRE_IN + z.n;
            b.src_pos = PRE_IN;
            b.dst = out;
            b.dst_size = sizeof(out);
            b.dst_pos = PRE_OUT;
            ZKT_STATUS(zu_decoder_process(d, &b, ZU_FINISH), ZU_STREAM_END);
            zu_decoder_free(d);
        }
        o.max_ratio = 0;
    }

    /* A cap larger than the caller's buffer leaves the buffer's own end in
       charge: the core may shrink the codec's window, never widen it. */
    {
        /* 500 bytes of room, cap 600: between the room and the room plus
           the offset, where getting the window from the wrong end of the
           buffer would let the codec write 100 bytes past it. */
        o.max_output = 600;
        zu_decoder *d = NULL;
        ZKT_STATUS(zu_decoder_new(&d, &o), ZU_OK);
        if (d != NULL) {
            memset(out, 0x5A, sizeof(out));
            zu_buffer b;
            memset(&b, 0, sizeof(b));
            b.src = in;
            b.src_size = PRE_IN + z.n;
            b.src_pos = PRE_IN;
            b.dst = out;
            b.dst_size = PRE_OUT + 500;
            b.dst_pos = PRE_OUT;
            /* The status is #108's subject (ZU_NEED_OUTPUT once it lands:
               output is owed); this check is about the window. */
            const zu_status ws = zu_decoder_process(d, &b, ZU_RUN);
            ZKT_CHECK(ws == ZU_NEED_OUTPUT || ws == ZU_NEED_INPUT);
            ZKT_CHECK(b.dst_size == PRE_OUT + 500);
            ZKT_CHECK(b.dst_pos == PRE_OUT + 500);
            int guard_ok = 1;
            for (size_t i = PRE_OUT + 500; i < PRE_OUT + 800; i++) {
                if (out[i] != 0x5A) { guard_ok = 0; }
            }
            ZKT_CHECK(guard_ok);
            zu_decoder_free(d);
        }
        o.max_output = 0;
    }

    /* identity copies what fits: a 4-byte window takes 4 of 10 bytes. */
    {
        zu_decoder_opts io;
        memset(&io, 0, sizeof(io));
        io.struct_size = (uint32_t) sizeof(io);
        io.codec = ZU_CODEC_IDENTITY;
        zu_decoder *d = NULL;
        ZKT_STATUS(zu_decoder_new(&d, &io), ZU_OK);
        if (d != NULL) {
            static const uint8_t ten[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
            uint8_t four[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
            zu_buffer b;
            memset(&b, 0, sizeof(b));
            b.src = ten;
            b.src_size = sizeof(ten);
            b.dst = four;
            b.dst_size = sizeof(four);
            ZKT_STATUS(zu_decoder_process(d, &b, ZU_RUN), ZU_NEED_OUTPUT);
            ZKT_CHECK(b.src_pos == 4 && b.dst_pos == 4);
            ZKT_CHECK(memcmp(four, ten, 4) == 0);
            zu_decoder_free(d);
        }
    }

    /* The one-shot entry points refuse what they cannot use. */
    {
        size_t w = 99;
        uint8_t one[1];
        o.max_output = 0;
        ZKT_STATUS(zu_decompress_one(&o, NULL, 5, one, 1, &w), ZU_ERR_INVALID_ARGUMENT);
        ZKT_STATUS(zu_decompress_one(&o, z.p, z.n, NULL, 8, &w), ZU_ERR_INVALID_ARGUMENT);
        ZKT_STATUS(zu_decompress_one(&o, z.p, z.n, one, 1, NULL), ZU_ERR_INVALID_ARGUMENT);
        /* NULL with a zero size is an empty buffer, which is legal. */
        zu_decoder_opts io;
        memset(&io, 0, sizeof(io));
        io.struct_size = (uint32_t) sizeof(io);
        io.codec = ZU_CODEC_IDENTITY;
        w = 99;
        ZKT_STATUS(zu_decompress_one(&io, NULL, 0, NULL, 0, &w), ZU_OK);
        ZKT_CHECK(w == 0);
        static uint8_t big[2 * N];
        ZKT_STATUS(zu_decompress_one(&o, z.p, z.n, big, sizeof(big), &w), ZU_OK);
        ZKT_CHECK(w == N);
        zu_encoder_opts eo;
        memset(&eo, 0, sizeof(eo));
        eo.struct_size = (uint32_t) sizeof(eo);
        eo.codec = ZU_CODEC_DEFLATE_RAW;
        eo.level = ZU_LEVEL_DEFAULT;
        ZKT_STATUS(zu_compress_one(&eo, NULL, 0, big, sizeof(big), &w), ZU_OK);
        ZKT_CHECK(w > 0);
        ZKT_STATUS(zu_compress_one(&eo, NULL, 3, big, sizeof(big), &w), ZU_ERR_INVALID_ARGUMENT);
    }
    zkt_bytes_free(&z);
}

void zkt_suite_limits(void)
{
    zkt_max_output();
    zkt_expected_size();
    zkt_max_ratio();
    zkt_trailing();
    zkt_members();
    zkt_entry_offsets();
}

/* -- input that is wrong --------------------------------------------------- */

/* Every proper prefix of a stream with an end of its own fails, and none
   returns success: truncation is never a short success. A missing wrapper
   field is ZU_ERR_TRUNCATED; a short DEFLATE body is invalid data (design 7,
   CLAUDE.md "ZU_FINISH arrives with the final bytes"). */
static void zkt_truncation(void)
{
    enum { N = 400 };
    static uint8_t text[N];
    zkt_payload_text(text, N);
    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zu_codec codec = zkt_codecs[c];
        if (codec == ZU_CODEC_IDENTITY) {
            continue;
        }
        zkt_bytes enc = {0}, got = {0};
        zkt_encode_ok(codec, text, N, &enc);
        for (size_t cut = 0; cut < enc.n; cut++) {
            zkt_drive_opts o = zkt_defaults();
            o.in_chunk = (cut % 2) ? 1 : 4096;
            o.out_chunk = (cut % 3) ? 4096 : 1;
            got.n = 0;
            zu_status st = zkt_decode(codec, enc.p, cut, &o, &got, NULL);
            int failed = (st == ZU_ERR_TRUNCATED || st == ZU_ERR_INVALID_DATA ||
                          st == ZU_ERR_CHECKSUM);
            /* An LZ4 block has no end marker, so a prefix that stops on a
               sequence boundary is itself a valid block. It may decode, but
               only to a strict prefix of the payload. */
            if (codec == ZU_CODEC_LZ4_BLOCK && st == ZU_OK) {
                failed = got.n < N && memcmp(got.p, text, got.n) == 0;
            }
            ZKT_CHECK(failed);
            if (!failed) {
                fprintf(stderr, "  codec %d prefix %zu of %zu: %s\n", (int) codec,
                        cut, enc.n, zu_status_string(st));
            }
        }
        /* The wrapper trailers: one byte short is truncation, not data. */
        if (codec == ZU_CODEC_ZLIB || codec == ZU_CODEC_GZIP) {
            zkt_drive_opts o = zkt_defaults();
            got.n = 0;
            ZKT_STATUS(zkt_decode(codec, enc.p, enc.n - 1, &o, &got, NULL),
                       ZU_ERR_TRUNCATED);
            got.n = 0;
            ZKT_STATUS(zkt_decode(codec, enc.p, 1, &o, &got, NULL), ZU_ERR_TRUNCATED);
        }
        zkt_bytes_free(&enc);
        zkt_bytes_free(&got);
    }
}

/* A corrupted checksum is ZU_ERR_CHECKSUM, distinctly (design 24 criterion
   6), and any single corrupted byte in a checksummed stream is an error or
   decodes to the payload -- never silently to something else. */
static void zkt_corruption(void)
{
    enum { N = 600 };
    static uint8_t text[N];
    zkt_payload_text(text, N);

    zkt_bytes z = {0}, g = {0}, got = {0};
    zkt_encode_ok(ZU_CODEC_ZLIB, text, N, &z);
    zkt_encode_ok(ZU_CODEC_GZIP, text, N, &g);
    zkt_drive_opts o = zkt_defaults();

    for (size_t i = 1; i <= 4; i++) {
        z.p[z.n - i] ^= 0x01;
        got.n = 0;
        ZKT_STATUS(zkt_decode(ZU_CODEC_ZLIB, z.p, z.n, &o, &got, NULL), ZU_ERR_CHECKSUM);
        z.p[z.n - i] ^= 0x01;
    }
    for (size_t i = 1; i <= 8; i++) {   /* CRC-32 then ISIZE */
        g.p[g.n - i] ^= 0x10;
        got.n = 0;
        ZKT_STATUS(zkt_decode(ZU_CODEC_GZIP, g.p, g.n, &o, &got, NULL), ZU_ERR_CHECKSUM);
        g.p[g.n - i] ^= 0x10;
    }
    /* The fixed header fields. */
    z.p[0] ^= 0x01;                      /* CM no longer 8, FCHECK broken */
    ZKT_STATUS(zkt_decode(ZU_CODEC_ZLIB, z.p, z.n, &o, &got, NULL), ZU_ERR_INVALID_DATA);
    z.p[0] ^= 0x01;
    for (size_t i = 0; i < 4; i++) {
        uint8_t keep = g.p[i];
        g.p[i] = (uint8_t) (i == 3 ? 0xe0 : keep ^ 0x40);  /* reserved FLG bits */
        ZKT_STATUS(zkt_decode(ZU_CODEC_GZIP, g.p, g.n, &o, &got, NULL),
                   ZU_ERR_INVALID_DATA);
        g.p[i] = keep;
    }

    for (size_t c = 0; c < ZKT_N_CODECS; c++) {
        zu_codec codec = zkt_codecs[c];
        if (codec == ZU_CODEC_IDENTITY || codec == ZU_CODEC_DEFLATE_RAW ||
            codec == ZU_CODEC_LZ4_BLOCK) {
            continue;    /* no checksum: a flip can decode to other bytes */
        }
        zkt_bytes enc = {0};
        zkt_encode_ok(codec, text, N, &enc);
        for (size_t i = 0; i < enc.n; i++) {
            for (int bit = 0; bit < 8; bit += 3) {
                enc.p[i] ^= (uint8_t) (1u << bit);
                zkt_drive_opts d = zkt_defaults();
                d.max_output = 4 * N;
                d.flags = ZU_DEC_REJECT_TRAILING;
                got.n = 0;
                zu_status st = zkt_decode(codec, enc.p, enc.n, &d, &got, NULL);
                int ok = (st != ZU_OK) || zkt_bytes_eq(&got, text, N);
                ZKT_CHECK(ok);
                if (!ok) {
                    fprintf(stderr, "  codec %d: flip at %zu bit %d decoded %zu "
                                    "wrong bytes\n", (int) codec, i, bit, got.n);
                }
                enc.p[i] ^= (uint8_t) (1u << bit);
            }
        }
        zkt_bytes_free(&enc);
    }
    zkt_bytes_free(&z);
    zkt_bytes_free(&g);
    zkt_bytes_free(&got);
}

/* Handles: argument checking, reset, use after the end. */
static void zkt_handles(void)
{
    enum { N = 1000 };
    static uint8_t text[N], out[4 * N];
    zkt_payload_text(text, N);

    zu_encoder_opts eo;
    memset(&eo, 0, sizeof(eo));
    eo.struct_size = (uint32_t) sizeof(eo);
    eo.codec = ZU_CODEC_GZIP;
    eo.level = ZU_LEVEL_DEFAULT;
    zu_decoder_opts dopt;
    memset(&dopt, 0, sizeof(dopt));
    dopt.struct_size = (uint32_t) sizeof(dopt);
    dopt.codec = ZU_CODEC_GZIP;

    zu_encoder *e = (zu_encoder *) &eo;   /* must be overwritten with NULL */
    zu_decoder *d = (zu_decoder *) &dopt;
    ZKT_STATUS(zu_encoder_new(NULL, &eo), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_encoder_new(&e, NULL), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decoder_new(NULL, &dopt), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decoder_new(&d, NULL), ZU_ERR_INVALID_ARGUMENT);

    /* struct_size: the required prefix, not the full current size. */
    zu_encoder_opts se = eo;
    se.struct_size = (uint32_t) ZU_ENCODER_OPTS_REQUIRED_SIZE - 1;
    ZKT_STATUS(zu_encoder_new(&e, &se), ZU_ERR_INVALID_ARGUMENT);
    ZKT_CHECK(e == NULL);
    se.struct_size = (uint32_t) ZU_ENCODER_OPTS_REQUIRED_SIZE;
    ZKT_STATUS(zu_encoder_new(&e, &se), ZU_OK);
    zu_encoder_free(e);

    zu_decoder_opts sd = dopt;
    sd.struct_size = (uint32_t) ZU_DECODER_OPTS_REQUIRED_SIZE - 1;
    ZKT_STATUS(zu_decoder_new(&d, &sd), ZU_ERR_INVALID_ARGUMENT);
    ZKT_CHECK(d == NULL);
    sd.struct_size = (uint32_t) ZU_DECODER_OPTS_REQUIRED_SIZE;
    ZKT_STATUS(zu_decoder_new(&d, &sd), ZU_OK);
    zu_decoder_free(d);
    /* The flag naming a field the struct does not contain is a caller bug,
       not "expected size 0". */
    sd.flags = ZU_DEC_EXPECTED_SIZE;
    ZKT_STATUS(zu_decoder_new(&d, &sd), ZU_ERR_INVALID_ARGUMENT);
    sd.struct_size = (uint32_t) (offsetof(zu_decoder_opts, expected_size) +
                                 sizeof(uint64_t) - 1);
    ZKT_STATUS(zu_decoder_new(&d, &sd), ZU_ERR_INVALID_ARGUMENT);
    sd.struct_size = (uint32_t) (offsetof(zu_decoder_opts, expected_size) +
                                 sizeof(uint64_t));
    sd.expected_size = 0;
    ZKT_STATUS(zu_decoder_new(&d, &sd), ZU_OK);
    zu_decoder_free(d);

    /* Unavailable and unknown codecs, and a level for a codec. */
    zu_encoder_opts bad = eo;
    bad.codec = ZU_CODEC_BROTLI;
    ZKT_STATUS(zu_encoder_new(&e, &bad), ZU_ERR_UNSUPPORTED);
    bad.codec = ZU_CODEC_NONE;
    ZKT_STATUS(zu_encoder_new(&e, &bad), ZU_ERR_UNSUPPORTED);
    zu_decoder_opts badd = dopt;
    badd.codec = (zu_codec) 999;
    ZKT_STATUS(zu_decoder_new(&d, &badd), ZU_ERR_UNSUPPORTED);
    size_t bound;
    ZKT_STATUS(zu_compress_bound(ZU_CODEC_BROTLI, ZU_LEVEL_DEFAULT, 10, &bound),
               ZU_ERR_UNSUPPORTED);
    ZKT_STATUS(zu_compress_bound(ZU_CODEC_GZIP, ZU_LEVEL_DEFAULT, 10, NULL),
               ZU_ERR_INVALID_ARGUMENT);

    /* process(): handle, buffer and flush validation. */
    ZKT_STATUS(zu_encoder_new(&e, &eo), ZU_OK);
    ZKT_STATUS(zu_decoder_new(&d, &dopt), ZU_OK);
    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));
    ZKT_STATUS(zu_encoder_process(NULL, &buf, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decoder_process(NULL, &buf, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_encoder_process(e, NULL, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decoder_process(d, NULL, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_encoder_process(e, &buf, (zu_flush) 3), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decoder_process(d, &buf, (zu_flush) -1), ZU_ERR_INVALID_ARGUMENT);
    buf.src = text; buf.src_size = 4; buf.src_pos = 5;
    ZKT_STATUS(zu_encoder_process(e, &buf, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decoder_process(d, &buf, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    buf.src_pos = 4;
    buf.dst = out; buf.dst_size = 2; buf.dst_pos = 3;
    ZKT_STATUS(zu_encoder_process(e, &buf, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decoder_process(d, &buf, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    memset(&buf, 0, sizeof(buf));
    buf.src_size = 1;                        /* NULL src with a size */
    ZKT_STATUS(zu_encoder_process(e, &buf, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    buf.src_size = 0;
    buf.dst_size = 1;                        /* NULL dst with a size */
    ZKT_STATUS(zu_decoder_process(d, &buf, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);
    /* Exactly-at-the-end cursors are valid, empty buffers. */
    memset(&buf, 0, sizeof(buf));
    buf.src = text; buf.src_size = 4; buf.src_pos = 4;
    buf.dst = out; buf.dst_size = 2; buf.dst_pos = 2;
    ZKT_CHECK(zu_encoder_process(e, &buf, ZU_RUN) != ZU_ERR_INVALID_ARGUMENT);

    /* A finished encoder: more input is a caller bug, none is STREAM_END. */
    zu_encoder_free(e);
    ZKT_STATUS(zu_encoder_new(&e, &eo), ZU_OK);
    memset(&buf, 0, sizeof(buf));
    buf.src = text; buf.src_size = N;
    buf.dst = out; buf.dst_size = sizeof(out);
    ZKT_STATUS(zu_encoder_process(e, &buf, ZU_FINISH), ZU_STREAM_END);
    size_t first_n = buf.dst_pos;
    ZKT_STATUS(zu_encoder_process(e, &buf, ZU_FINISH), ZU_STREAM_END);
    ZKT_CHECK(buf.dst_pos == first_n);
    buf.src_pos = 0;
    ZKT_STATUS(zu_encoder_process(e, &buf, ZU_RUN), ZU_ERR_INVALID_ARGUMENT);

    /* Reset re-runs the same codec; it cannot switch codecs, and the
       output is the same stream again. */
    zu_encoder_opts other = eo;
    other.codec = ZU_CODEC_ZLIB;
    ZKT_STATUS(zu_encoder_reset(e, &other), ZU_ERR_INVALID_ARGUMENT);
    zu_encoder_opts lv = eo;
    lv.level = 99;
    ZKT_STATUS(zu_encoder_reset(e, &lv), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_encoder_reset(NULL, &eo), ZU_ERR_INVALID_ARGUMENT);
    zu_encoder_opts shrt = eo;
    shrt.struct_size = 4;
    ZKT_STATUS(zu_encoder_reset(e, &shrt), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_encoder_reset(e, NULL), ZU_OK);
    static uint8_t again[4 * N];
    zu_buffer b2;
    memset(&b2, 0, sizeof(b2));
    b2.src = text; b2.src_size = N;
    b2.dst = again; b2.dst_size = sizeof(again);
    ZKT_STATUS(zu_encoder_process(e, &b2, ZU_FINISH), ZU_STREAM_END);
    ZKT_CHECK(b2.dst_pos == first_n && memcmp(again, out, first_n) == 0);

    /* A finished decoder: trailing input is the policy's call. */
    static uint8_t dec[2 * N];
    zu_decoder_free(d);
    zu_decoder_opts strict = dopt;
    strict.flags = ZU_DEC_REJECT_TRAILING;
    strict.max_output = N;
    ZKT_STATUS(zu_decoder_new(&d, &strict), ZU_OK);
    memset(&b2, 0, sizeof(b2));
    b2.src = out; b2.src_size = first_n;
    b2.dst = dec; b2.dst_size = sizeof(dec);
    ZKT_STATUS(zu_decoder_process(d, &b2, ZU_FINISH), ZU_STREAM_END);
    ZKT_CHECK(b2.dst_pos == N && memcmp(dec, text, N) == 0);
    ZKT_STATUS(zu_decoder_process(d, &b2, ZU_FINISH), ZU_STREAM_END);
    b2.src_pos = first_n - 1;
    ZKT_STATUS(zu_decoder_process(d, &b2, ZU_FINISH), ZU_ERR_TRAILING);

    /* Reset starts a fresh budget: a second message of max_output bytes on
       the same handle passes. */
    zu_decoder_opts dz = dopt;
    dz.codec = ZU_CODEC_ZLIB;
    ZKT_STATUS(zu_decoder_reset(d, &dz), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decoder_reset(NULL, &strict), ZU_ERR_INVALID_ARGUMENT);
    zu_decoder_opts dshort = strict;
    dshort.struct_size = 4;
    ZKT_STATUS(zu_decoder_reset(d, &dshort), ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decoder_reset(d, NULL), ZU_OK);
    memset(&b2, 0, sizeof(b2));
    b2.src = out; b2.src_size = first_n;
    b2.dst = dec; b2.dst_size = sizeof(dec);
    ZKT_STATUS(zu_decoder_process(d, &b2, ZU_FINISH), ZU_STREAM_END);
    ZKT_CHECK(b2.dst_pos == N && memcmp(dec, text, N) == 0);
    /* And the reset's own options take effect. */
    zu_decoder_opts tighter = strict;
    tighter.max_output = N - 1;
    ZKT_STATUS(zu_decoder_reset(d, &tighter), ZU_OK);
    memset(&b2, 0, sizeof(b2));
    b2.src = out; b2.src_size = first_n;
    b2.dst = dec; b2.dst_size = sizeof(dec);
    ZKT_STATUS(zu_decoder_process(d, &b2, ZU_FINISH), ZU_ERR_OUTPUT_LIMIT);

    zu_encoder_free(e);
    zu_decoder_free(d);
    zu_encoder_free(NULL);
    zu_decoder_free(NULL);

    /* One-shot argument checking. */
    size_t w = 7;
    ZKT_STATUS(zu_compress_one(&eo, text, N, out, sizeof(out), NULL),
               ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_compress_one(&eo, NULL, 1, out, sizeof(out), &w),
               ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_decompress_one(&dopt, out, 1, NULL, 1, &w),
               ZU_ERR_INVALID_ARGUMENT);
    ZKT_STATUS(zu_compress_one(&bad, text, N, out, sizeof(out), &w),
               ZU_ERR_UNSUPPORTED);
    ZKT_CHECK(w == 0);
}

void zkt_suite_errors(void)
{
    zkt_truncation();
    zkt_corruption();
    zkt_handles();
}
