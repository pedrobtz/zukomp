/* Oracles for the fuzz targets (#68): drives that keep what a decode
 * delivered, and reference decoders that know the answer.
 *
 * A target that decodes and returns 0 is a crash oracle. It cannot see a
 * wrong byte, a wrong verdict or a verdict that changes with the chunking.
 * The match-distance bug fixed by miniz patch 0002 returned heap bytes as
 * data, and #64 rejected a valid stream at the cap. Neither crashed. So
 * every target here compares a zukomp run with something that has to agree
 * with it, and abort()s when they differ. libFuzzer and the replay driver
 * both report an abort as a crash.
 *
 * The references:
 *
 *   - system zlib for deflate-raw, zlib and gzip. It is linked into the fuzz
 *     build only, never the package (design 22 #11, #36);
 *   - libzstd's one-shot ZSTD_decompressDCtx() for zstd. It does not stream,
 *     so it shares no code with zu_frames.c's prober or the stall rules;
 *   - liblz4's frame decoder given the whole input at once, for lz4-frame;
 *   - LZ4_decompress_safe() at the block's exact decoded size, for
 *     lz4-block. That is the one capacity at which liblz4 enforces the
 *     block format's end-of-block rules (see zu_fuzz_ref_lz4_block()).
 *
 * The comparisons are of the verdict (accepted or refused) and the bytes,
 * never of the error class. Two decoders may name the same fault
 * differently, and a stream with two faults fails at whichever one each
 * reaches first.
 *
 * This header is fuzz-only, so it may print. Nothing in it is linked into
 * zukomp. */
#ifndef ZU_FUZZ_ORACLE_H
#define ZU_FUZZ_ORACLE_H

#include <stdio.h>

#include <zlib.h>

#include "fuzz_common.h"
#define ZSTD_STATIC_LINKING_ONLY   /* ZSTD_getFrameHeader(), ZSTD_decompressBound() */
#include "zstd.h"
#include "zstd_errors.h"
#include "lz4.h"
#include "lz4frame.h"

/* System zlib is not built under MSan, so it writes our buffers, and
   returns its values, without updating their shadow. Unpoisoning what it
   wrote and returned keeps MSan's reports about zukomp's bytes, which are
   the only ones it is here for. */
#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
#    include <sanitizer/msan_interface.h>
#    define ZU_FUZZ_UNPOISON(p, n) __msan_unpoison((p), (n))
#  endif
#endif
#ifndef ZU_FUZZ_UNPOISON
#  define ZU_FUZZ_UNPOISON(p, n) ((void) 0)
#endif

/* An oracle failure. It prints what disagreed, then aborts. */
#define ZU_FUZZ_FAIL(...)                                                  \
    do {                                                                   \
        fprintf(stderr, "zukomp fuzz oracle: " __VA_ARGS__);               \
        fputc('\n', stderr);                                               \
        abort();                                                           \
    } while (0)
#define ZU_FUZZ_CHECK(cond, ...)                                           \
    do {                                                                   \
        if (!(cond)) {                                                     \
            ZU_FUZZ_FAIL(__VA_ARGS__);                                     \
        }                                                                  \
    } while (0)

/* The derived passes (round trips, limit straddles) only run below this
   output size. They decode the same bytes several more times, so this keeps
   the fuzzer's throughput on the inputs it is there for. */
#define ZU_FUZZ_DERIVED_MAX (1u << 20)

/* -- what a run delivered ---------------------------------------------- */

typedef struct {
    zu_status st;       /* ZU_OK for a stream that ended, else the error */
    uint8_t  *out;      /* every byte delivered, in order */
    size_t    len;
    size_t    cap;      /* allocated size of out */
    const char *why;    /* a reference's own reason for refusing, if any */
} zu_fuzz_run;

static void zu_fuzz_run_free(zu_fuzz_run *r)
{
    free(r->out);
    memset(r, 0, sizeof(*r));
}

/* Room for `extra` more bytes. The harness running out of memory says
   nothing about zukomp, so it is not an oracle failure; it ends the input. */
static void zu_fuzz_reserve(zu_fuzz_run *r, size_t extra)
{
    if (r->cap - r->len >= extra) {
        return;
    }
    size_t want = r->cap ? r->cap : 4096;
    while (want - r->len < extra) {
        want *= 2;
    }
    uint8_t *p = (uint8_t *) realloc(r->out, want);
    if (p == NULL) {
        fprintf(stderr, "fuzz harness: out of memory\n");
        exit(0);
    }
    r->out = p;
    r->cap = want;
}

static int zu_fuzz_accepted(const zu_fuzz_run *r) { return r->st == ZU_OK; }

/* -- driving zukomp ------------------------------------------------------ */

typedef struct {
    zu_codec codec;
    size_t   in_chunk;          /* 0: all input in one buffer */
    size_t   out_chunk;         /* 0: ZU_FUZZ_OUT_CHUNK */
    uint32_t flags;             /* ZU_DEC_* */
    uint64_t max_output;        /* 0: ZU_FUZZ_MAX_OUTPUT */
    int      has_expected;
    uint64_t expected;
    /* Which call carries ZU_FINISH (#81). 1: the call that hands over the
       final bytes, as zu_int_run_whole() does for komp_decompress(). 0: a
       later call with an empty buffer, as a streaming consumer that learns
       of the end afterwards does. The DEFLATE adapter treats a first-call
       FINISH specially, so the two are different paths. */
    int      finish_with_last;
} zu_fuzz_dec;

/* Decodes `data` under `o`, keeping every delivered byte.
 *
 * A run that cannot make progress is recorded as ZU_ERR_INTERNAL, as
 * zu_int_run_whole() records it, and the targets refuse that outcome: a
 * decoder told ZU_FINISH that still asks for input, or one that moves
 * nothing call after call, is a zukomp bug whatever the input. */
static void zu_fuzz_decode_run(const zu_fuzz_dec *o, const uint8_t *data,
                               size_t size, zu_fuzz_run *r)
{
    zu_fuzz_init();
    memset(r, 0, sizeof(*r));

    zu_decoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec = o->codec;
    opts.max_output = o->max_output ? o->max_output : ZU_FUZZ_MAX_OUTPUT;
    opts.flags = o->flags | (o->has_expected ? ZU_DEC_EXPECTED_SIZE : 0u);
    opts.expected_size = o->expected;

    zu_decoder *dec = NULL;
    zu_status st = zu_decoder_new(&dec, &opts);
    if (st != ZU_OK) {
        ZU_FUZZ_FAIL("zu_decoder_new(codec %d) failed: %s", (int) o->codec,
                     zu_status_string(st));
    }

    const size_t in_chunk  = o->in_chunk ? o->in_chunk : (size ? size : 1);
    const size_t out_chunk = o->out_chunk ? o->out_chunk : ZU_FUZZ_OUT_CHUNK;
    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));
    size_t fed = 0;
    int stalls = 0;

    for (;;) {
        if (buf.src_pos == buf.src_size && fed < size) {
            size_t take = size - fed;
            if (take > in_chunk) { take = in_chunk; }
            buf.src = data + fed;
            buf.src_size = take;
            buf.src_pos = 0;
            fed += take;
        }
        const int last = o->finish_with_last
            ? (fed >= size)
            : (fed >= size && buf.src_pos == buf.src_size);

        zu_fuzz_reserve(r, out_chunk);
        buf.dst = r->out + r->len;
        buf.dst_size = out_chunk;
        buf.dst_pos = 0;
        const size_t before_in = buf.src_pos;

        st = zu_decoder_process(dec, &buf, last ? ZU_FINISH : ZU_RUN);
        zu_fuzz_consume(buf.dst, buf.dst_pos);
        r->len += buf.dst_pos;

        if (st == ZU_STREAM_END) { r->st = ZU_OK; break; }
        if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
            r->st = st;
            break;
        }
        const int moved = buf.dst_pos > 0 || buf.src_pos > before_in;
        if (last && st == ZU_NEED_INPUT && !moved) {
            r->st = ZU_ERR_INTERNAL;    /* told FINISH, still wants input */
            break;
        }
        /* Input left in the buffer but nothing taken or given, twice: a
           refill cannot help, since the driver refills only when the
           buffer is spent. */
        stalls = moved ? 0 : stalls + 1;
        if (stalls > 2 && buf.src_pos < buf.src_size) {
            r->st = ZU_ERR_INTERNAL;
            break;
        }
        if (stalls > 64) {
            r->st = ZU_ERR_INTERNAL;
            break;
        }
    }
    zu_decoder_free(dec);
}

/* The decode every whole-stream target starts from: all input in one
   buffer, FINISH in the given position, the 16 MiB cap. */
static void zu_fuzz_decode_whole(zu_codec codec, uint32_t flags,
                                 int finish_with_last, const uint8_t *data,
                                 size_t size, zu_fuzz_run *r)
{
    zu_fuzz_dec o;
    memset(&o, 0, sizeof(o));
    o.codec = codec;
    o.flags = flags;
    o.finish_with_last = finish_with_last;
    zu_fuzz_decode_run(&o, data, size, r);
}

/* Encodes `data`. flush_every > 0 sends ZU_FLUSH on every nth call that is
   not the last (only for codecs that advertise it). The encoder must
   succeed on any input: a failure is an oracle failure. */
static void zu_fuzz_encode_run(zu_codec codec, int32_t level,
                               const uint8_t *data, size_t size,
                               size_t in_chunk, size_t out_chunk,
                               unsigned flush_every, int finish_with_last,
                               zu_fuzz_run *r)
{
    zu_fuzz_init();
    memset(r, 0, sizeof(*r));

    zu_encoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec = codec;
    opts.level = level;
    zu_encoder *enc = NULL;
    zu_status st = zu_encoder_new(&enc, &opts);
    if (st != ZU_OK) {
        ZU_FUZZ_FAIL("zu_encoder_new(codec %d, level %d) failed: %s",
                     (int) codec, (int) level, zu_status_string(st));
    }
    if (in_chunk == 0) { in_chunk = size ? size : 1; }
    if (out_chunk == 0) { out_chunk = ZU_FUZZ_OUT_CHUNK; }

    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));
    size_t fed = 0;
    unsigned long calls = 0;
    int stalls = 0;
    for (;;) {
        if (buf.src_pos == buf.src_size && fed < size) {
            size_t take = size - fed;
            if (take > in_chunk) { take = in_chunk; }
            buf.src = data + fed;
            buf.src_size = take;
            buf.src_pos = 0;
            fed += take;
        }
        const int last = finish_with_last
            ? (fed >= size)
            : (fed >= size && buf.src_pos == buf.src_size);
        zu_flush flush = last ? ZU_FINISH : ZU_RUN;
        if (!last && flush_every > 0 && ((calls + 1) % flush_every) == 0) {
            flush = ZU_FLUSH;
        }
        zu_fuzz_reserve(r, out_chunk);
        buf.dst = r->out + r->len;
        buf.dst_size = out_chunk;
        buf.dst_pos = 0;
        const size_t before_in = buf.src_pos;
        st = zu_encoder_process(enc, &buf, flush);
        r->len += buf.dst_pos;
        calls++;
        if (st == ZU_STREAM_END) { r->st = ZU_OK; break; }
        if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
            ZU_FUZZ_FAIL("encoder (codec %d, level %d) failed: %s",
                         (int) codec, (int) level, zu_status_string(st));
        }
        const int moved = buf.dst_pos > 0 || buf.src_pos > before_in;
        stalls = moved ? 0 : stalls + 1;
        if (stalls > 64) {
            ZU_FUZZ_FAIL("encoder (codec %d) made no progress", (int) codec);
        }
    }
    zu_encoder_free(enc);
}

/* -- comparisons ---------------------------------------------------------- */

static int zu_fuzz_same_bytes(const zu_fuzz_run *a, const zu_fuzz_run *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(a->out, b->out, a->len) == 0);
}

/* `a` delivered nothing beyond a prefix of what `b` delivered. */
static int zu_fuzz_is_prefix(const zu_fuzz_run *a, const zu_fuzz_run *b)
{
    return a->len <= b->len && (a->len == 0 || memcmp(a->out, b->out, a->len) == 0);
}

/* The common prefix of `a` and `b` is the same bytes. */
static int zu_fuzz_agree_so_far(const zu_fuzz_run *a, const zu_fuzz_run *b)
{
    size_t n = a->len < b->len ? a->len : b->len;
    return n == 0 || memcmp(a->out, b->out, n) == 0;
}

/* Where `a` and `b` first differ, for the failure messages. */
static size_t zu_fuzz_first_diff(const zu_fuzz_run *a, const zu_fuzz_run *b)
{
    size_t n = a->len < b->len ? a->len : b->len;
    for (size_t i = 0; i < n; i++) {
        if (a->out[i] != b->out[i]) { return i; }
    }
    return n;
}

/* Both runs reached the same verdict with the same bytes. */
#define ZU_FUZZ_SAME(a, b, what)                                           \
    ZU_FUZZ_CHECK((a)->st == (b)->st && zu_fuzz_same_bytes((a), (b)),      \
                  "%s: %s with %zu bytes vs %s with %zu bytes", (what),    \
                  zu_status_string((a)->st), (a)->len,                     \
                  zu_status_string((b)->st), (b)->len)

/* -- an independent reading of the gzip header (RFC 1952 section 2.3) ------
 *
 * Written from the RFC, not from zu_gzip.c: a whole-buffer reader that
 * indexes into the bytes, where zukomp's parser is a byte-at-a-time state
 * machine. They share nothing but the specification; the CRC is zlib's.
 *
 * Returns ZU_OK with *len set to the header length, ZU_NEED_INPUT when the
 * bytes end inside the header, or the error zukomp is specified to raise:
 * ZU_ERR_INVALID_DATA for a malformed header and ZU_ERR_CHECKSUM for a
 * failed FHCRC. max_field is zukomp's own bound on FNAME and FCOMMENT
 * (ZU_INT_GZIP_MAX_FIELD), which the RFC does not have. It is a refusal
 * zukomp documents, so the reference applies it too. Pass 0 for none. */
static zu_status zu_fuzz_ref_gzip_header(const uint8_t *p, size_t n,
                                         size_t max_field, size_t *len)
{
    *len = 0;
    /* ID1, ID2, CM, FLG, in order: the parser rejects at the first byte
       that is wrong, so a short input with a wrong byte is invalid, not
       incomplete. */
    static const uint8_t lead[3] = { 0x1F, 0x8B, 0x08 };
    for (size_t i = 0; i < 3; i++) {
        if (n <= i) { return ZU_NEED_INPUT; }
        if (p[i] != lead[i]) { return ZU_ERR_INVALID_DATA; }
    }
    if (n <= 3) { return ZU_NEED_INPUT; }
    const uint8_t flg = p[3];
    if (flg & 0xE0) { return ZU_ERR_INVALID_DATA; }     /* reserved bits */
    size_t at = 10;                                     /* MTIME XFL OS */
    if (n < at) { return ZU_NEED_INPUT; }
    if (flg & 0x04) {                                   /* FEXTRA */
        if (n < at + 2) { return ZU_NEED_INPUT; }
        size_t xlen = (size_t) p[at] | ((size_t) p[at + 1] << 8);
        at += 2 + xlen;
        if (n < at) { return ZU_NEED_INPUT; }
    }
    for (int field = 0; field < 2; field++) {           /* FNAME, FCOMMENT */
        if (!(flg & (field == 0 ? 0x08 : 0x10))) { continue; }
        const uint8_t *nul = (const uint8_t *) memchr(p + at, 0, n - at);
        size_t flen = nul ? (size_t) (nul - (p + at)) : n - at;
        if (max_field && flen > max_field) { return ZU_ERR_INVALID_DATA; }
        if (nul == NULL) { return ZU_NEED_INPUT; }
        at += flen + 1;
    }
    if (flg & 0x02) {                                   /* FHCRC */
        if (n < at + 2) { return ZU_NEED_INPUT; }
        unsigned long crc = crc32(crc32(0L, Z_NULL, 0), p, (uInt) at);
        ZU_FUZZ_UNPOISON(&crc, sizeof(crc));
        unsigned want = (unsigned) p[at] | ((unsigned) p[at + 1] << 8);
        if (want != (crc & 0xFFFFu)) { return ZU_ERR_CHECKSUM; }
        at += 2;
    }
    *len = at;
    return ZU_OK;
}

/* -- reference: system zlib, for the DEFLATE family ---------------------- */

/* Decodes with zlib under the same 16 MiB cap, applying zukomp's member and
   trailing-bytes policy (ZU_DEC_CONCAT_MEMBERS | ZU_DEC_REJECT_TRAILING):
   another gzip member follows only on both magic bytes, and anything else
   after the stream is refused. *member_starts receives where each gzip
   member began, for the FNAME/FCOMMENT excuse in the targets.
 *
 * st is ZU_OK, ZU_ERR_OUTPUT_LIMIT (more than the cap) or
 * ZU_ERR_INVALID_DATA (zlib refused, the input ended early, or junk
 * followed). */
#define ZU_FUZZ_MAX_MEMBERS 64
static void zu_fuzz_ref_zlib(zu_codec codec, const uint8_t *data, size_t size,
                             zu_fuzz_run *r, size_t *member_starts,
                             size_t *n_members)
{
    memset(r, 0, sizeof(*r));
    *n_members = 0;
    const int wbits = (codec == ZU_CODEC_GZIP) ? 15 + 16
                    : (codec == ZU_CODEC_ZLIB) ? 15 : -15;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    int init = inflateInit2(&zs, wbits);
    ZU_FUZZ_UNPOISON(&init, sizeof(init));
    if (init != Z_OK) {
        fprintf(stderr, "fuzz harness: inflateInit2 failed\n");
        exit(0);
    }
    const size_t cap = ZU_FUZZ_MAX_OUTPUT;
    size_t pos = 0;
    member_starts[(*n_members)++] = 0;
    for (;;) {
        zu_fuzz_reserve(r, ZU_FUZZ_OUT_CHUNK);
        size_t room = r->cap - r->len;
        if (room > cap + 1 - r->len) { room = cap + 1 - r->len; }
        zs.next_in   = (Bytef *) (data + pos);
        zs.avail_in  = (uInt) (size - pos);
        zs.next_out  = r->out + r->len;
        zs.avail_out = (uInt) room;
        int rc = inflate(&zs, Z_NO_FLUSH);
        ZU_FUZZ_UNPOISON(&rc, sizeof(rc));
        ZU_FUZZ_UNPOISON(&zs, sizeof(zs));
        const size_t used = (size - pos) - zs.avail_in;
        const size_t made = room - zs.avail_out;
        ZU_FUZZ_UNPOISON(r->out + r->len, made);
        pos += used;
        r->len += made;
        if (r->len > cap) {
            r->len = cap;
            r->st = ZU_ERR_OUTPUT_LIMIT;
            break;
        }
        if (rc == Z_STREAM_END) {
            if (codec == ZU_CODEC_GZIP && size - pos >= 2 &&
                data[pos] == 0x1F && data[pos + 1] == 0x8B &&
                *n_members < ZU_FUZZ_MAX_MEMBERS) {
                member_starts[(*n_members)++] = pos;
                inflateReset(&zs);
                continue;
            }
            r->st = (pos == size) ? ZU_OK : ZU_ERR_TRAILING;
            break;
        }
        if (rc == Z_OK && (used > 0 || made > 0)) {
            continue;
        }
        if (rc == Z_BUF_ERROR && zs.avail_out == 0) {
            continue;                   /* out of room: grow and go on */
        }
        /* Z_DATA_ERROR, Z_NEED_DICT (zukomp: FDICT unsupported), or no
           progress with all input given: the stream is short. zlib's
           messages are static strings. */
        r->st = ZU_ERR_INVALID_DATA;
        r->why = zs.msg ? zs.msg : "input ended inside the stream";
        break;
    }
    inflateEnd(&zs);
}

/* What libzstd's streaming decoder delivers given everything at once, with
   no window ceiling: the prefix reference for a refused zstd stream. */
static void zu_fuzz_ref_zstd_stream(const uint8_t *data, size_t size,
                                    zu_fuzz_run *r)
{
    memset(r, 0, sizeof(*r));
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (dctx == NULL) { exit(0); }
    /* libzstd's default ceiling, 2^27: above it zukomp refuses the window
       too, and a larger one would let a header claim gigabytes here. */
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 27);
    ZSTD_inBuffer in = { data, size, 0 };
    r->st = ZU_ERR_INVALID_DATA;
    for (;;) {
        zu_fuzz_reserve(r, ZU_FUZZ_OUT_CHUNK);
        ZSTD_outBuffer out = { r->out + r->len, r->cap - r->len, 0 };
        size_t rc = ZSTD_decompressStream(dctx, &out, &in);
        r->len += out.pos;
        if (ZSTD_isError(rc)) { break; }
        if (r->len > ZU_FUZZ_MAX_OUTPUT) {
            r->len = ZU_FUZZ_MAX_OUTPUT;
            r->st = ZU_ERR_OUTPUT_LIMIT;
            break;
        }
        if (out.pos == 0 && in.pos == in.size) { break; }
    }
    ZSTD_freeDCtx(dctx);
}

/* -- reference: libzstd's one-shot decoder ------------------------------- */

/* ZSTD_decompressDCtx() over the whole input into the cap. It decodes
   every frame, steps over skippable frames, and refuses trailing bytes
   that are not a frame: zukomp's policy under ZU_DEC_CONCAT_MEMBERS |
   ZU_DEC_REJECT_TRAILING. It returns no bytes on failure, so on a refusal
   out is empty; the targets compare prefixes against the streaming
   reference below instead. */
static void zu_fuzz_ref_zstd(const uint8_t *data, size_t size, zu_fuzz_run *r)
{
    memset(r, 0, sizeof(*r));
    /* RFC 8878 section 3: compressed data "is made of one or more frames".
       ZSTD_decompressDCtx() returns success for no input at all, which is
       zero frames; zukomp refuses it as truncated, which the RFC supports,
       so the reference does too. */
    if (size == 0) {
        r->st = ZU_ERR_TRUNCATED;
        return;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (dctx == NULL) { exit(0); }
    /* Size the buffer from the frames' declared sizes where they say, so a
       small stream does not cost a 16 MiB allocation per input. */
    unsigned long long want = ZSTD_decompressBound(data, size);
    size_t room = (want == ZSTD_CONTENTSIZE_ERROR || want > ZU_FUZZ_MAX_OUTPUT)
                ? ZU_FUZZ_MAX_OUTPUT : (size_t) want;
    zu_fuzz_reserve(r, room ? room : 1);
    size_t got = ZSTD_decompressDCtx(dctx, r->out, room, data, size);
    if (ZSTD_isError(got) &&
        ZSTD_getErrorCode(got) == ZSTD_error_dstSize_tooSmall &&
        room < ZU_FUZZ_MAX_OUTPUT) {
        /* The bound was wrong for this input; retry at the cap. */
        zu_fuzz_reserve(r, ZU_FUZZ_MAX_OUTPUT);
        got = ZSTD_decompressDCtx(dctx, r->out, ZU_FUZZ_MAX_OUTPUT, data, size);
    }
    if (!ZSTD_isError(got)) {
        r->len = got;
        r->st = ZU_OK;
    } else if (ZSTD_getErrorCode(got) == ZSTD_error_dstSize_tooSmall) {
        /* Too small for what the frame header *declares*, which is said
           before a byte of the data is checked. Whether the stream really
           runs past the cap is the streaming decoder's to say. */
        zu_fuzz_run st;
        zu_fuzz_ref_zstd_stream(data, size, &st);
        r->st = (st.st == ZU_ERR_OUTPUT_LIMIT) ? ZU_ERR_OUTPUT_LIMIT
                                               : ZU_ERR_INVALID_DATA;
        zu_fuzz_run_free(&st);
    } else {
        r->st = ZU_ERR_INVALID_DATA;
    }
    ZSTD_freeDCtx(dctx);
}

/* -- reference: liblz4's frame decoder, whole input ---------------------- */

static void zu_fuzz_ref_lz4_frame(const uint8_t *data, size_t size,
                                  zu_fuzz_run *r)
{
    memset(r, 0, sizeof(*r));
    LZ4F_dctx *dctx = NULL;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION))) {
        exit(0);
    }
    size_t pos = 0;
    size_t hint = 1;
    for (;;) {
        zu_fuzz_reserve(r, ZU_FUZZ_OUT_CHUNK);
        size_t room = r->cap - r->len;
        if (room > ZU_FUZZ_MAX_OUTPUT + 1 - r->len) {
            room = ZU_FUZZ_MAX_OUTPUT + 1 - r->len;
        }
        size_t dsize = room, ssize = size - pos;
        hint = LZ4F_decompress(dctx, r->out + r->len, &dsize,
                               data + pos, &ssize, NULL);
        if (LZ4F_isError(hint)) {
            r->st = ZU_ERR_INVALID_DATA;
            break;
        }
        pos += ssize;
        r->len += dsize;
        if (r->len > ZU_FUZZ_MAX_OUTPUT) {
            r->len = ZU_FUZZ_MAX_OUTPUT;
            r->st = ZU_ERR_OUTPUT_LIMIT;
            break;
        }
        if (hint == 0 && pos == size) {
            r->st = ZU_OK;              /* the last frame ended with the input */
            break;
        }
        if (ssize == 0 && dsize == 0) {
            /* All input given and the frame wants more, or bytes after a
               frame that liblz4 will not take. */
            r->st = ZU_ERR_INVALID_DATA;
            break;
        }
    }
    LZ4F_freeDecompressionContext(dctx);
}

/* -- reference: one LZ4 block -------------------------------------------- *
 *
 * The block format says the last five bytes are literals and the last
 * match starts at least twelve bytes before the end of the block.
 * LZ4_decompress_safe() checks both against the end of the *output buffer*,
 * not the end of the decoded data, so a block that breaks them decodes in
 * a buffer a few bytes larger than its size and is refused in one exactly
 * its size (measured on liblz4 1.10.0). The reference therefore learns the
 * size from a decode at the cap and then decodes again at exactly that
 * size, which is the verdict the format defines. */
static void zu_fuzz_ref_lz4_block(const uint8_t *data, size_t size,
                                  zu_fuzz_run *r)
{
    memset(r, 0, sizeof(*r));
    r->st = ZU_ERR_INVALID_DATA;
    if (size == 0 || size > (size_t) LZ4_MAX_INPUT_SIZE) {
        return;
    }
    /* One input byte decodes to at most 255. */
    size_t room = size * 255 + 64;
    if (room > ZU_FUZZ_MAX_OUTPUT) { room = ZU_FUZZ_MAX_OUTPUT; }
    zu_fuzz_reserve(r, room);
    int n = LZ4_decompress_safe((const char *) data, (char *) r->out,
                                (int) size, (int) room);
    if (n < 0) {
        int p = LZ4_decompress_safe_partial((const char *) data,
                                            (char *) r->out, (int) size,
                                            (int) room, (int) room);
        if (p >= 0 && (size_t) p == room && room == ZU_FUZZ_MAX_OUTPUT) {
            /* Well-formed for the whole cap and still going. Whether it is
               larger or corrupt beyond the cap, no 16 MiB reader can say. */
            r->len = (size_t) p;
            r->st = ZU_ERR_OUTPUT_LIMIT;
        }
        return;
    }
    /* For n = 0 too: a lone token 07 (a match with no offset) "decodes"
       to nothing in a large buffer, and is refused in an empty one. */
    int exact = LZ4_decompress_safe((const char *) data, (char *) r->out,
                                    (int) size, n);
    if (exact != n) {
        return;                         /* breaks the end-of-block rules */
    }
    r->len = (size_t) n;
    r->st = ZU_OK;
}

/* -- the whole-stream oracle, shared by the six fuzz_decode_* targets ---- */

/* zukomp's zstd window ceiling under the fuzz cap: ceil(log2(16 MiB)) = 24,
   inside codec_zstd.c's [23, 27]. */
#define ZU_FUZZ_ZSTD_WINDOWLOG 24

/* Some frame in `data` declares a window larger than zukomp will allocate
   under the fuzz cap. zukomp refuses that frame before decoding it (design
   20: memory the input merely claims), where one-shot decoding needs no
   window at all and accepts it. */
static int zu_fuzz_zstd_window_too_large(const uint8_t *data, size_t size,
                                         int wlog)
{
    size_t off = 0;
    while (off < size) {
        ZSTD_frameHeader h;
        if (ZSTD_getFrameHeader(&h, data + off, size - off) != 0) {
            return 0;
        }
        if (h.frameType == ZSTD_frame &&
            h.windowSize > ((unsigned long long) 1 << wlog)) {
            return 1;
        }
        size_t c = ZSTD_findFrameCompressedSize(data + off, size - off);
        if (ZSTD_isError(c) || c == 0) {
            return 0;
        }
        off += c;
    }
    return 0;
}

/* Some gzip member header has an FNAME or FCOMMENT longer than zukomp's
   ZU_INT_GZIP_MAX_FIELD, which zukomp refuses and zlib does not bound. */
static int zu_fuzz_gzip_field_too_long(const uint8_t *data, size_t size,
                                       const size_t *starts, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        size_t len;
        if (zu_fuzz_ref_gzip_header(data + starts[i], size - starts[i],
                                    ZU_INT_GZIP_MAX_FIELD, &len) ==
                ZU_ERR_INVALID_DATA &&
            zu_fuzz_ref_gzip_header(data + starts[i], size - starts[i], 0,
                                    &len) != ZU_ERR_INVALID_DATA) {
            return 1;
        }
    }
    return 0;
}

/* The #64 and #74 exemptions below are off when ZU_FUZZ_NO_EXEMPTIONS is
   set, which is how a fix is checked: with it, the regress-64-* and
   regress-74-* seeds must abort before the fix and replay clean after it,
   and then the exemption is deleted. */
static int zu_fuzz_exemptions_on(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("ZU_FUZZ_NO_EXEMPTIONS") == NULL;
    }
    return on;
}

/* EXEMPTION, to remove when #74 is fixed. miniz builds a Huffman code
   without checking that it is complete or that the literal/length code has
   an end-of-block symbol, where zlib refuses both (RFC 1951 section 3.2.7
   and zlib's inflate.c). So for such a stream miniz decodes on, delivering
   bytes zlib never produces, until it trips over something else or the
   cap. Past the end of the input tinfl reads zero bits, which is how a
   short dynamic header reaches the same state.
 *
   The shape, and nothing wider: a DEFLATE codec, both sides refuse, the
   bytes agree as far as zlib got and zukomp got further, and zlib's own
   reason is one of its incomplete-code messages, on the input or on the
   input followed by the zero bits tinfl assumes.
   regress-74-missing-eob sits inside it. */
static int zu_fuzz_issue74_reason(const char *why)
{
    static const char *const code_msgs[] = {
        "invalid code -- missing end-of-block", "invalid code lengths set",
        "invalid literal/lengths set", "invalid distances set",
        "invalid literal/length/distance code", "invalid literal/length code",
        "invalid distance code", "invalid bit length repeat"
    };
    for (size_t i = 0; why && i < sizeof(code_msgs) / sizeof(code_msgs[0]); i++) {
        if (strcmp(why, code_msgs[i]) == 0) { return 1; }
    }
    return 0;
}

static int zu_fuzz_issue74(zu_codec codec, const uint8_t *data, size_t size,
                           const zu_fuzz_run *a, const zu_fuzz_run *ref)
{
    if (!zu_fuzz_exemptions_on() ||
        !(codec == ZU_CODEC_GZIP || codec == ZU_CODEC_ZLIB ||
          codec == ZU_CODEC_DEFLATE_RAW) ||
        !(a->st == ZU_ERR_INVALID_DATA || a->st == ZU_ERR_OUTPUT_LIMIT) ||
        ref->st != ZU_ERR_INVALID_DATA || a->len <= ref->len ||
        !zu_fuzz_agree_so_far(a, ref)) {
        return 0;
    }
    if (zu_fuzz_issue74_reason(ref->why)) {
        return 1;
    }
    uint8_t *padded = (uint8_t *) calloc(size + 64, 1);
    if (padded == NULL) { return 0; }
    if (size) { memcpy(padded, data, size); }
    zu_fuzz_run z;
    size_t starts[ZU_FUZZ_MAX_MEMBERS], n;
    zu_fuzz_ref_zlib(codec, padded, size + 64, &z, starts, &n);
    int hit = zu_fuzz_issue74_reason(z.why);
    zu_fuzz_run_free(&z);
    free(padded);
    return hit;
}

/* Compresses `plain` with zukomp and requires both zukomp and the reference
   to decode it back (a round trip, and an external decoder reading zukomp's
   output). */
static void zu_fuzz_roundtrip_check(zu_codec codec, int32_t level,
                                    const uint8_t *plain, size_t n,
                                    size_t in_chunk, size_t out_chunk,
                                    unsigned flush_every, int finish_with_last);

static void zu_fuzz_reference(zu_codec codec, const uint8_t *data, size_t size,
                              zu_fuzz_run *ref, size_t *starts, size_t *nstarts)
{
    *nstarts = 0;
    switch (codec) {
    case ZU_CODEC_GZIP:
    case ZU_CODEC_ZLIB:
    case ZU_CODEC_DEFLATE_RAW:
        zu_fuzz_ref_zlib(codec, data, size, ref, starts, nstarts);
        break;
    case ZU_CODEC_ZSTD:       zu_fuzz_ref_zstd(data, size, ref); break;
    case ZU_CODEC_LZ4_FRAME:  zu_fuzz_ref_lz4_frame(data, size, ref); break;
    case ZU_CODEC_LZ4_BLOCK:  zu_fuzz_ref_lz4_block(data, size, ref); break;
    default:
        ZU_FUZZ_FAIL("no reference decoder for codec %d", (int) codec);
    }
}

/* A level to re-encode at, chosen by the input so the corpus covers the
   range. Kept to levels that are quick: this runs on every accepted input. */
static int32_t zu_fuzz_pick_level(zu_codec codec, unsigned k)
{
    static const int32_t deflate[] = { 0, 1, 6, 9 };
    static const int32_t zstd[]    = { -5, 1, 3, 9 };
    static const int32_t lz4[]     = { -8, 1, 3, 9 };
    switch (codec) {
    case ZU_CODEC_GZIP: case ZU_CODEC_ZLIB: case ZU_CODEC_DEFLATE_RAW:
        return deflate[k % 4];
    case ZU_CODEC_ZSTD:
        return zstd[k % 4];
    case ZU_CODEC_LZ4_FRAME: case ZU_CODEC_LZ4_BLOCK:
        return lz4[k % 4];
    default:
        return ZU_LEVEL_DEFAULT;
    }
}

/* Every whole-stream decode target runs this:
 *
 *   1. FINISH ORDER (#81). The same input with ZU_FINISH on the call that
 *      carries the final bytes (komp_decompress()'s order) and on a later
 *      empty call: same verdict, same bytes.
 *   2. REFERENCE. zukomp and the codec's reference agree on accept or
 *      refuse; on accept, on every byte; past the cap, both stop there.
 *   3. VALID PREFIX. On a refusal, what zukomp delivered is a prefix of
 *      what the reference decoded. It never hands out bytes the stream does
 *      not contain, which is the shape of the match-distance bug.
 *   4. ROUND TRIP. Accepted output re-encoded by zukomp decodes back to
 *      itself, through zukomp and through the reference.
 *
 * Two refusals zukomp documents and the reference does not have are
 * excused, each only in its exact shape: a gzip FNAME or FCOMMENT longer
 * than ZU_INT_GZIP_MAX_FIELD, and a zstd frame whose window exceeds the
 * ceiling derived from max_output. */
static void zu_fuzz_whole_stream_oracle(zu_codec codec, const uint8_t *data,
                                        size_t size)
{
    const uint32_t flags = ZU_DEC_CONCAT_MEMBERS | ZU_DEC_REJECT_TRAILING;
    zu_fuzz_run a, b, ref;
    zu_fuzz_decode_whole(codec, flags, 1, data, size, &a);
    zu_fuzz_decode_whole(codec, flags, 0, data, size, &b);
    ZU_FUZZ_CHECK(a.st != ZU_ERR_INTERNAL && b.st != ZU_ERR_INTERNAL,
                  "codec %d: the decoder stalled (%s / %s)", (int) codec,
                  zu_status_string(a.st), zu_status_string(b.st));
    ZU_FUZZ_SAME(&a, &b, "FINISH with the last bytes vs on a later call");

    size_t starts[ZU_FUZZ_MAX_MEMBERS], nstarts = 0;
    zu_fuzz_reference(codec, data, size, &ref, starts, &nstarts);

    if (ref.st == ZU_OK) {
        if (a.st == ZU_OK) {
            ZU_FUZZ_CHECK(zu_fuzz_same_bytes(&a, &ref),
                          "codec %d: accepted with %zu bytes, the reference "
                          "decoded %zu different ones", (int) codec, a.len,
                          ref.len);
        } else if (codec == ZU_CODEC_GZIP && a.st == ZU_ERR_INVALID_DATA &&
                   zu_fuzz_gzip_field_too_long(data, size, starts, nstarts)) {
            ZU_FUZZ_CHECK(zu_fuzz_is_prefix(&a, &ref),
                          "gzip: refused a long FNAME after wrong bytes");
        } else if (codec == ZU_CODEC_ZSTD && a.st == ZU_ERR_OUTPUT_LIMIT &&
                   zu_fuzz_zstd_window_too_large(data, size,
                                                 ZU_FUZZ_ZSTD_WINDOWLOG)) {
            ZU_FUZZ_CHECK(zu_fuzz_is_prefix(&a, &ref),
                          "zstd: refused a large window after wrong bytes");
        } else {
            ZU_FUZZ_FAIL("codec %d: refused (%s, %zu bytes out) a stream the "
                         "reference decodes to %zu bytes", (int) codec,
                         zu_status_string(a.st), a.len, ref.len);
        }
    } else if (ref.st == ZU_ERR_OUTPUT_LIMIT) {
        ZU_FUZZ_CHECK(a.st == ZU_ERR_OUTPUT_LIMIT,
                      "codec %d: the reference passes the 16 MiB cap, zukomp "
                      "says %s after %zu bytes", (int) codec,
                      zu_status_string(a.st), a.len);
    } else {
        ZU_FUZZ_CHECK(a.st != ZU_OK,
                      "codec %d: accepted (%zu bytes) a stream the reference "
                      "refuses", (int) codec, a.len);
    }

    /* The valid-prefix check, for every refusal. zstd's one-shot reference
       returns no bytes on failure, so its prefix comes from libzstd's own
       streaming decoder given everything at once; an LZ4 block that is
       refused yields nothing at all. */
    if (a.st != ZU_OK) {
        zu_fuzz_run pre;
        memset(&pre, 0, sizeof(pre));
        const zu_fuzz_run *p = &ref;
        if (codec == ZU_CODEC_ZSTD) {
            zu_fuzz_ref_zstd_stream(data, size, &pre);
            p = &pre;
        }
        if (zu_fuzz_issue74(codec, data, size, &a, p)) {
            /* excused: see zu_fuzz_issue74() */
        } else if (codec == ZU_CODEC_LZ4_BLOCK) {
            ZU_FUZZ_CHECK(a.len == 0 || zu_fuzz_is_prefix(&a, p),
                          "lz4-block: refused, yet delivered %zu bytes",
                          a.len);
        } else {
            ZU_FUZZ_CHECK(zu_fuzz_is_prefix(&a, p),
                          "codec %d: refused (%s) after delivering %zu bytes "
                          "that are not a prefix of the reference's %zu (%s); "
                          "first difference at %zu", (int) codec,
                          zu_status_string(a.st), a.len, p->len,
                          zu_status_string(p->st), zu_fuzz_first_diff(&a, p));
        }
        zu_fuzz_run_free(&pre);
    }

    if (a.st == ZU_OK && a.len <= ZU_FUZZ_DERIVED_MAX) {
        unsigned k = (unsigned) (size + (size ? data[size - 1] : 0));
        zu_fuzz_roundtrip_check(codec, zu_fuzz_pick_level(codec, k), a.out,
                                a.len, 0, 0, 0, (int) (k & 1));
    }
    zu_fuzz_run_free(&a);
    zu_fuzz_run_free(&b);
    zu_fuzz_run_free(&ref);
}

static void zu_fuzz_roundtrip_check(zu_codec codec, int32_t level,
                                    const uint8_t *plain, size_t n,
                                    size_t in_chunk, size_t out_chunk,
                                    unsigned flush_every, int finish_with_last)
{
    zu_fuzz_run enc, dec;
    zu_fuzz_encode_run(codec, level, plain, n, in_chunk, out_chunk,
                       flush_every, finish_with_last, &enc);

    /* zu_compress_bound() is an upper bound for an encode without
       ZU_FLUSH: every sync flush adds an empty stored block (zu_whole.c). */
    if (flush_every == 0) {
        size_t bound = 0;
        ZU_FUZZ_CHECK(zu_compress_bound(codec, level, n, &bound) == ZU_OK,
                      "zu_compress_bound(codec %d) failed", (int) codec);
        ZU_FUZZ_CHECK(enc.len <= bound,
                      "codec %d level %d: %zu bytes encoded to %zu, over the "
                      "bound %zu", (int) codec, (int) level, n, enc.len, bound);
    }

    zu_fuzz_dec o;
    memset(&o, 0, sizeof(o));
    o.codec = codec;
    o.flags = ZU_DEC_REJECT_TRAILING;
    o.finish_with_last = 1;
    /* lz4-block carries no size; the caller of a block always knows it. */
    if (codec == ZU_CODEC_LZ4_BLOCK) {
        o.has_expected = 1;
        o.expected = n;
    }
    zu_fuzz_decode_run(&o, enc.out, enc.len, &dec);
    ZU_FUZZ_CHECK(dec.st == ZU_OK && dec.len == n &&
                  (n == 0 || memcmp(dec.out, plain, n) == 0),
                  "codec %d level %d: %zu bytes did not round-trip (%s, %zu "
                  "bytes back)", (int) codec, (int) level, n,
                  zu_status_string(dec.st), dec.len);
    zu_fuzz_run_free(&dec);

    if (codec != ZU_CODEC_IDENTITY) {
        zu_fuzz_run ref;
        size_t starts[ZU_FUZZ_MAX_MEMBERS], nstarts;
        zu_fuzz_reference(codec, enc.out, enc.len, &ref, starts, &nstarts);
        ZU_FUZZ_CHECK(ref.st == ZU_OK && ref.len == n &&
                      (n == 0 || memcmp(ref.out, plain, n) == 0),
                      "codec %d level %d: the reference cannot read zukomp's "
                      "encoding of %zu bytes (%s, %zu bytes back)",
                      (int) codec, (int) level, n, zu_status_string(ref.st),
                      ref.len);
        zu_fuzz_run_free(&ref);
    }
    zu_fuzz_run_free(&enc);
}

#endif /* ZU_FUZZ_ORACLE_H */
