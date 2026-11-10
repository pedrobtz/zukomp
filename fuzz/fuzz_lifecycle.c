/* The encoder and decoder handles' API, as a state machine checked against
 * a model of design 9's handle lifecycle (decision 20, #71).
 *
 * Every other target drives a decoder front to back and stops at the first
 * terminal status, so a bug that needs an out-of-order sequence is out of
 * their reach: process after an error, after the end, ZU_RUN after
 * ZU_FINISH, a reset or a free part-way. Here the fuzzer picks the codec,
 * the options and a sequence of operations on one handle, and every call is
 * checked against what the contract says it must return:
 *
 *   - after a failure, every process() returns that status and moves
 *     neither cursor, whatever it passes, until a reset;
 *   - a refused call (no buffer, ZU_FLUSH where the codec cannot, ZU_RUN
 *     after ZU_FINISH, input to an ended encoder) moves nothing and changes
 *     nothing;
 *   - after the end an empty call is ZU_STREAM_END again, and decoder input
 *     is ZU_ERR_TRAILING -- a failure -- under ZU_DEC_REJECT_TRAILING;
 *   - an injected codec failure (zu_int_*_inject(), where a codec's own
 *     status enters) is handled like any other;
 *   - reset, and free followed by a new handle, start a fresh stream.
 *
 * And against two oracles that know the answer:
 *
 *   - the one-shot API. What a decoder has delivered is always a prefix of
 *     what zu_decompress_one() decodes from the same bytes; a clean stream
 *     decoded through the handle ends exactly as the one-shot call does,
 *     same status, same bytes; and what the encoder handle wrote decodes,
 *     one-shot, to exactly the input it consumed;
 *   - an allocation balance: every byte the core and the vendored libraries
 *     allocated for this input is freed by the end of it, whatever the
 *     sequence (the sanitizer runtime's own count, so it sees inside miniz,
 *     libzstd and liblz4, not only the core's handles).
 *
 * Input layout:
 *   [0] codec, mod 7
 *   [1] bit 0 ZU_DEC_REJECT_TRAILING, bit 1 ZU_DEC_CONCAT_MEMBERS,
 *       bit 2 a small max_output, bit 3 corrupt one byte of the stream,
 *       bit 4 append three bytes of junk, bit 5 ZU_DEC_EXPECTED_SIZE,
 *       bit 6 decode the encoder handle's own output, bit 7 x16 payload
 *   [2] a parameter: the small max_output, the expected-size offset, the
 *       corrupted position, the first junk byte
 *   [3] payload length; [4] number of encoder operations
 *   the payload, then two bytes per operation: the first picks the
 *   operation (low three bits) and the output room (the rest, x8, +1), the
 *   second is the input offered. The encoder's operations come first. */
#include "fuzz_common.h"

#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#    define ZU_FUZZ_ALLOC_STATS 1
#  endif
#endif
#if !defined(ZU_FUZZ_ALLOC_STATS) && defined(__SANITIZE_ADDRESS__)
#  define ZU_FUZZ_ALLOC_STATS 1
#endif
#ifdef ZU_FUZZ_ALLOC_STATS
#  include <sanitizer/allocator_interface.h>
#  define ZU_FUZZ_ALLOCATED() __sanitizer_get_current_allocated_bytes()
#else
#  define ZU_FUZZ_ALLOCATED() ((size_t) 0)
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* A broken rule names its line, so a crasher says which rule it broke. */
#include <stdio.h>
static void lc_fail(int line)
{
    fprintf(stderr, "fuzz_lifecycle: contract violated at line %d\n", line);
    abort();
}
#define LC_FAIL() lc_fail(__LINE__)

#define LC_PAYLOAD_CAP 4096u
#define LC_ENC_CAP     (1u << 17)
#define LC_DEC_CAP     (1u << 20)   /* the decoders' max_output, so a bomb
                                       stops at the cap, not at OOM */

/* Static, so the harness itself allocates nothing the balance would see. */
static uint8_t lc_payload[LC_PAYLOAD_CAP];
static uint8_t lc_enc[LC_ENC_CAP];
static uint8_t lc_stream[LC_ENC_CAP + 3];
static uint8_t lc_ref[LC_DEC_CAP];
static uint8_t lc_got[LC_DEC_CAP];

static const zu_codec lc_codecs[7] = {
    ZU_CODEC_IDENTITY, ZU_CODEC_DEFLATE_RAW, ZU_CODEC_ZLIB, ZU_CODEC_GZIP,
    ZU_CODEC_ZSTD, ZU_CODEC_LZ4_FRAME, ZU_CODEC_LZ4_BLOCK
};
static const zu_status lc_injectable[5] = {
    ZU_ERR_MEMORY, ZU_ERR_INTERNAL, ZU_ERR_INVALID_DATA, ZU_ERR_CHECKSUM,
    ZU_ERR_TRUNCATED
};

enum { LC_RUN, LC_FLUSH, LC_FINISH, LC_INJECT, LC_RESET, LC_NULLBUF,
       LC_EMPTY, LC_RENEW };

typedef enum { LC_OPEN, LC_ENDED, LC_FAILED } lc_state;

typedef struct {
    lc_state  state;
    zu_status failed;
    int       finishing;
    int       inject_pending;
    zu_status inject;
    int       injected;      /* this stream saw an injection */
    size_t    cursor;        /* input consumed by this stream */
    size_t    out;           /* output this stream delivered */
    /* The input this stream has been told it has: all of it until a call
       passes ZU_FINISH, and from then on what that call offered. ZU_FINISH
       means no more input follows (design 8), so a caller never offers more
       afterwards -- and the stream it describes is that prefix, which is
       what the one-shot reference decodes. */
    size_t    limit;
    int       limited;
} lc_model;

static int lc_is_error(zu_status st) { return st >= ZU_ERR_INVALID_ARGUMENT; }

static void lc_fresh(lc_model *m)
{
    memset(m, 0, sizeof(*m));
    m->state = LC_OPEN;
}

static size_t lc_limit(const lc_model *m, size_t n_src)
{
    return m->limited ? m->limit : n_src;
}

/* One process() call, checked against the model. `in` and `room` are what
   the caller offers; NULL `buf` is the null-buffer refusal. Returns what the
   call returned. */
static zu_status lc_call(int encode, zu_encoder *e, zu_decoder *d,
                         lc_model *m, const uint8_t *src, size_t n_src,
                         size_t in, size_t room, uint8_t *sink, size_t cap,
                         zu_flush flush, int nullbuf, int can_flush,
                         uint32_t dflags)
{
    /* After ZU_FINISH a caller presents exactly the remainder it declared;
       offering less would be declaring a different end. */
    const size_t avail = lc_limit(m, n_src) - m->cursor;
    if (in > avail || m->limited) { in = avail; }
    if (room > cap - m->out) { room = cap - m->out; }

    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.src      = (in == 0) ? NULL : src + m->cursor;
    buf.src_size = in;
    buf.dst      = (room == 0) ? NULL : sink + m->out;
    buf.dst_size = room;

    zu_buffer *b = nullbuf ? NULL : &buf;
    zu_status st = encode ? zu_encoder_process(e, b, flush)
                          : zu_decoder_process(d, b, flush);
    const size_t used = buf.src_pos, made = buf.dst_pos;
    if (used > in || made > room) { LC_FAIL(); }

    /* Rule 1: a failed stream answers with its failure, moving nothing. */
    if (m->state == LC_FAILED) {
        if (st != m->failed || used != 0 || made != 0) { LC_FAIL(); }
        return st;
    }
    /* Rule 2: refusals before the stream sees the call. */
    if (nullbuf) {
        if (st != ZU_ERR_INVALID_ARGUMENT) { LC_FAIL(); }
        return st;
    }
    if (m->state == LC_ENDED) {
        if (used != 0 || made != 0) { LC_FAIL(); }
        if (in == 0) {
            if (st != ZU_STREAM_END) { LC_FAIL(); }
        } else if (encode) {
            if (st != ZU_ERR_INVALID_ARGUMENT) { LC_FAIL(); }
        } else if (dflags & ZU_DEC_REJECT_TRAILING) {
            /* Rule 4: trailing input after the end is a failure. */
            if (st != ZU_ERR_TRAILING) { LC_FAIL(); }
            m->state = LC_FAILED;
            m->failed = st;
        } else if (st != ZU_STREAM_END) {
            LC_FAIL();
        }
        return st;
    }
    if (m->finishing && flush != ZU_FINISH) {
        /* Rule 3: ZU_FINISH is final. */
        if (st != ZU_ERR_INVALID_ARGUMENT || used != 0 || made != 0) { LC_FAIL(); }
        return st;
    }
    if (flush == ZU_FLUSH && !can_flush) {
        if (st != ZU_ERR_UNSUPPORTED || used != 0 || made != 0) { LC_FAIL(); }
        return st;
    }

    /* The call reached the stream. */
    if (flush == ZU_FINISH) {
        m->finishing = 1;
        if (!m->limited) {
            m->limited = 1;
            m->limit   = m->cursor + in;
        }
    }
    if (m->inject_pending) {
        m->inject_pending = 0;
        m->injected = 1;
        /* The injection replaces the codec's call, so nothing moves. A
           decoder may still report a limit that was already spent instead,
           which is the core's precedence, not the codec's. */
        if (used != 0 || made != 0 || !lc_is_error(st)) { LC_FAIL(); }
        if (encode && st != m->inject) { LC_FAIL(); }
    }
    m->cursor += used;
    m->out    += made;
    if (lc_is_error(st)) {
        m->state  = LC_FAILED;
        m->failed = st;
    } else if (st == ZU_STREAM_END) {
        /* An encoder ends only on ZU_FINISH. */
        if (encode && flush != ZU_FINISH) { LC_FAIL(); }
        m->state = LC_ENDED;
    } else if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
        LC_FAIL();
    }
    /* Rule 5: an empty call moves no input. */
    if (in == 0 && used != 0) { LC_FAIL(); }
    return st;
}

/* Drives an open stream to its end with ZU_FINISH and all the room left. */
static void lc_drain(int encode, zu_encoder *e, zu_decoder *d, lc_model *m,
                     const uint8_t *src, size_t n_src, uint8_t *sink,
                     size_t cap, int can_flush, uint32_t dflags)
{
    for (int i = 0; i < 256 && m->state == LC_OPEN; i++) {
        const size_t before_in = m->cursor, before_out = m->out;
        zu_status st = lc_call(encode, e, d, m, src, n_src, n_src, cap, sink,
                               cap, ZU_FINISH, 0, can_flush, dflags);
        if (m->state != LC_OPEN) { break; }
        if (st == ZU_NEED_OUTPUT && m->out == cap) { break; }   /* sink full */
        if (m->cursor == before_in && m->out == before_out) { break; }
    }
}

/* The common prefix of what was delivered and what the one-shot call
   decoded must be all of the shorter one. */
static void lc_prefix(const uint8_t *a, size_t na, const uint8_t *b, size_t nb)
{
    size_t n = (na < nb) ? na : nb;
    if (n > 0 && memcmp(a, b, n) != 0) { LC_FAIL(); }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zu_fuzz_init();
    if (size < 5) { return 0; }

    const zu_codec codec  = lc_codecs[data[0] % 7];
    const uint8_t  bits   = data[1];
    const uint8_t  param  = data[2];
    size_t         plen   = data[3];
    size_t         n_enc  = data[4];
    const uint8_t *p      = data + 5;
    size_t         left   = size - 5;
    if (plen > left) { plen = left; }

    /* The payload, repeated sixteen-fold under bit 7 to reach past a
       block. */
    const size_t reps = (bits & 0x80) ? 16 : 1;
    const size_t n = plen * reps;
    for (size_t i = 0; i < n; i++) { lc_payload[i] = p[i % plen]; }
    p += plen;
    left -= plen;
    const size_t nops = left / 2;
    if (n_enc > nops) { n_enc = nops; }

    zu_codec_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t) sizeof(info);
    if (zu_codec_get_info(codec, &info) != ZU_OK) { LC_FAIL(); }
    const int can_flush = (info.flags & ZU_CAN_FLUSH) != 0;

    const size_t baseline = ZU_FUZZ_ALLOCATED();

    /* -- the encoder ------------------------------------------------------ */

    zu_encoder_opts eo;
    memset(&eo, 0, sizeof(eo));
    eo.struct_size = (uint32_t) sizeof(eo);
    eo.codec = codec;
    eo.level = ZU_LEVEL_DEFAULT;

    zu_encoder *enc = NULL;
    if (zu_encoder_new(&enc, &eo) != ZU_OK) { LC_FAIL(); }
    lc_model em;
    lc_fresh(&em);

    for (size_t i = 0; i < n_enc; i++) {
        const uint8_t opb = p[2 * i], arg = p[2 * i + 1];
        const size_t room = (size_t) (opb >> 3) * 8u + 1u;
        switch (opb & 7) {
        case LC_RUN: case LC_FLUSH: case LC_FINISH: case LC_EMPTY: {
            const int k = opb & 7;
            const zu_flush fl = (k == LC_FLUSH) ? ZU_FLUSH
                              : (k == LC_FINISH) ? ZU_FINISH : ZU_RUN;
            lc_call(1, enc, NULL, &em, lc_payload, n,
                    (k == LC_EMPTY) ? 0 : (size_t) arg * reps, room,
                    lc_enc, LC_ENC_CAP, fl, 0, can_flush, 0);
            break;
        }
        case LC_INJECT:
            /* Only an open stream will reach its codec again, so only
               there is the injection pending; an ended or failed handle
               keeps it until a reset clears it. */
            zu_int_encoder_inject(enc, lc_injectable[arg % 5]);
            if (em.state == LC_OPEN) {
                em.inject_pending = 1;
                em.inject = lc_injectable[arg % 5];
            }
            break;
        case LC_RESET:
            if (zu_encoder_reset(enc, &eo) != ZU_OK) { LC_FAIL(); }
            lc_fresh(&em);
            break;
        case LC_NULLBUF:
            lc_call(1, enc, NULL, &em, lc_payload, n, 0, 0, lc_enc,
                    LC_ENC_CAP, (zu_flush) (arg % 3), 1, can_flush, 0);
            break;
        default:   /* LC_RENEW: free part-way, start again on a new handle */
            zu_encoder_free(enc);
            enc = NULL;
            if (zu_encoder_new(&enc, &eo) != ZU_OK) { LC_FAIL(); }
            lc_fresh(&em);
            break;
        }
    }
    /* A pending injection would fire in the drain; it is part of the
       model, so the drain checks it like any other call. */
    lc_drain(1, enc, NULL, &em, lc_payload, n, lc_enc, LC_ENC_CAP,
             can_flush, 0);

    /* Oracle: what the encoder handle wrote decodes, one-shot, to exactly
       the input it consumed. */
    int enc_clean = (em.state == LC_ENDED);
    if (enc_clean) {
        zu_decoder_opts o;
        memset(&o, 0, sizeof(o));
        o.struct_size = (uint32_t) sizeof(o);
        o.codec = codec;
        o.flags = ZU_DEC_REJECT_TRAILING;
        if (codec == ZU_CODEC_LZ4_BLOCK) {
            o.flags |= ZU_DEC_EXPECTED_SIZE;
            o.expected_size = em.cursor;
        }
        size_t w = 0;
        zu_status st = zu_decompress_one(&o, lc_enc, em.out, lc_ref,
                                         LC_DEC_CAP, &w);
        if (st != ZU_OK || w != em.cursor ||
            (w > 0 && memcmp(lc_ref, lc_payload, w) != 0)) {
            LC_FAIL();
        }
    } else if (em.state == LC_OPEN || !em.injected) {
        /* Only an injection makes a built-in encoder fail, and nothing
           stops a drain with the whole output buffer to write into. */
        LC_FAIL();
    }
    zu_encoder_free(enc);

    /* -- the stream to decode ---------------------------------------------- */

    size_t ns;
    if ((bits & 0x40) && enc_clean && em.cursor == n) {
        ns = em.out;
        memcpy(lc_stream, lc_enc, ns);
    } else {
        size_t w = 0;
        if (zu_compress_one(&eo, lc_payload, n, lc_stream, LC_ENC_CAP, &w)
                != ZU_OK) {
            LC_FAIL();
        }
        ns = w;
    }
    const int corrupt = (bits & 0x08) && ns > 0;
    if (corrupt) { lc_stream[param % ns] ^= (uint8_t) (0x01u << (param % 8)); }
    if (bits & 0x10) {
        lc_stream[ns++] = param;
        lc_stream[ns++] = 0x00;
        lc_stream[ns++] = 0x42;
    }

    zu_decoder_opts dopts;
    memset(&dopts, 0, sizeof(dopts));
    dopts.struct_size = (uint32_t) sizeof(dopts);
    dopts.codec = codec;
    dopts.flags = (uint32_t) (bits & 0x03);
    dopts.max_output = (bits & 0x04) ? (uint64_t) param + 1u : LC_DEC_CAP;
    if (bits & 0x20) {
        dopts.flags |= ZU_DEC_EXPECTED_SIZE;
        const size_t off = param % 3;          /* n - 1, n, n + 1 */
        dopts.expected_size = (n + off >= 1) ? n + off - 1 : 0;
    }
    const size_t dcap = (size_t) dopts.max_output;

    /* The references. `full` is the stream's content as one call sees it,
       with no limit and no trailing policy; `ref` is the one-shot answer
       under the very options the handle has. */
    zu_decoder_opts fo = dopts;
    fo.flags &= ~(uint32_t) (ZU_DEC_REJECT_TRAILING | ZU_DEC_EXPECTED_SIZE);
    fo.max_output = LC_DEC_CAP;
    size_t nfull = 0;
    zu_status full_st = zu_decompress_one(&fo, lc_stream, ns, lc_ref,
                                          LC_DEC_CAP, &nfull);
    (void) full_st;

    /* -- the decoder ------------------------------------------------------ */

    zu_decoder *dec = NULL;
    if (zu_decoder_new(&dec, &dopts) != ZU_OK) { LC_FAIL(); }
    lc_model dm;
    lc_fresh(&dm);

    const size_t n_dec = nops - n_enc;
    const uint8_t *dp = p + 2 * n_enc;
    for (size_t i = 0; i < n_dec; i++) {
        const uint8_t opb = dp[2 * i], arg = dp[2 * i + 1];
        const size_t room = (size_t) (opb >> 3) * 8u + 1u;
        switch (opb & 7) {
        case LC_RUN: case LC_FLUSH: case LC_FINISH: case LC_EMPTY: {
            const int k = opb & 7;
            const zu_flush fl = (k == LC_FLUSH) ? ZU_FLUSH
                              : (k == LC_FINISH) ? ZU_FINISH : ZU_RUN;
            lc_call(0, NULL, dec, &dm, lc_stream, ns,
                    (k == LC_EMPTY) ? 0 : (size_t) arg, room,
                    lc_got, dcap, fl, 0, can_flush, dopts.flags);
            break;
        }
        case LC_INJECT:
            zu_int_decoder_inject(dec, lc_injectable[arg % 5]);
            if (dm.state == LC_OPEN) {
                dm.inject_pending = 1;
                dm.inject = lc_injectable[arg % 5];
            }
            break;
        case LC_RESET:
            if (zu_decoder_reset(dec, &dopts) != ZU_OK) { LC_FAIL(); }
            lc_fresh(&dm);
            break;
        case LC_NULLBUF:
            lc_call(0, NULL, dec, &dm, lc_stream, ns, 0, 0, lc_got, dcap,
                    (zu_flush) (arg % 3), 1, can_flush, dopts.flags);
            break;
        default:
            zu_decoder_free(dec);
            dec = NULL;
            if (zu_decoder_new(&dec, &dopts) != ZU_OK) { LC_FAIL(); }
            lc_fresh(&dm);
            break;
        }
        /* Oracle: the stream's delivered bytes agree with the one-shot
           decode of the same bytes for as far as both go. */
        lc_prefix(lc_got, dm.out, lc_ref, nfull);
    }
    lc_drain(0, NULL, dec, &dm, lc_stream, ns, lc_got, dcap, can_flush,
             dopts.flags);
    /* A stream can end before the caller has offered everything it has --
       a DEFLATE body ends on its final block under ZU_RUN. The caller then
       offers the rest, and the end-of-stream rules decide what it is. */
    if (dm.state == LC_ENDED && dm.cursor < lc_limit(&dm, ns)) {
        lc_call(0, NULL, dec, &dm, lc_stream, ns, ns, 0, lc_got, dcap,
                ZU_FINISH, 0, can_flush, dopts.flags);
    }
    lc_prefix(lc_got, dm.out, lc_ref, nfull);
    zu_fuzz_consume(lc_got, dm.out);

    /* Oracle: a clean stream ends through the handle exactly as it does in
       one call. Clean means unmodified, with no limit small enough to bind
       and no expected size -- the cases #64 and #80 already record as
       depending on the delivery -- and no injection in this stream. */
    if (!corrupt && !(bits & 0x24) && !dm.injected && dm.state != LC_OPEN) {
        size_t nref = 0;
        zu_status ref_st = zu_decompress_one(&dopts, lc_stream,
                                             lc_limit(&dm, ns), lc_ref,
                                             LC_DEC_CAP, &nref);
        if (ref_st == ZU_OK) {
            if (dm.state != LC_ENDED || dm.out != nref ||
                (nref > 0 && memcmp(lc_got, lc_ref, nref) != 0)) {
                LC_FAIL();
            }
        } else if (dm.state != LC_FAILED || dm.failed != ref_st) {
            LC_FAIL();
        }
    }
    zu_decoder_free(dec);

    /* Oracle: everything allocated for this input was freed. */
    if (ZU_FUZZ_ALLOCATED() != baseline) { LC_FAIL(); }
    return 0;
}
