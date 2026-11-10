/* Splits its own input, using the first bytes as the chunk sizes.
 *
 * Chunk-boundary handling is the property zuhttp depends on and the one
 * that rots silently, and it is not reachable by a fuzzer that always hands
 * over the whole buffer at once. Letting the fuzzer choose the split makes
 * the state machines' resumption paths part of the search space.
 *
 * Byte 0 is the input chunk (1..256), byte 1 the output chunk (1..256),
 * byte 2 the codec (byte 2 % 7) and which call carries ZU_FINISH ((byte 2 /
 * 7) & 1: with the final bytes, or on a later empty call; #81).
 *
 * The oracles (#68):
 *
 *   1. CHUNKED AGAINST WHOLE. The split decode and a decode of the whole
 *      input in one buffer reach the same verdict with the same bytes. A
 *      chunking that drops, repeats or invents a byte at a boundary, or a
 *      stall misclassified at one, fails this.
 *   2. LIMITS AT THE DECODED SIZE N (the shape of #64), at the same split:
 *      max_output = N and expected_size = N decode to the same bytes;
 *      max_output = N - 1 is ZU_ERR_OUTPUT_LIMIT; expected_size = N - 1 and
 *      N + 1 are ZU_ERR_INVALID_DATA. A refusal delivers only a prefix of
 *      the N bytes.
 *
 * zstd's window ceiling follows max_output (codec_zstd.c), so a frame that
 * declares a window above it is refused at a small max_output however
 * small its content. That refusal is documented policy and is excused,
 * only for a frame that declares such a window. */
#include "fuzz_oracle.h"

static int zu_fuzz_ceil_log2(uint64_t n)
{
    int b = 0;
    while (b < 63 && ((uint64_t) 1 << b) < n) { b++; }
    return b;
}

/* The ceiling codec_zstd.c derives from a max_output. */
static int zu_fuzz_zstd_wlog(uint64_t max_output)
{
    int w = zu_fuzz_ceil_log2(max_output);
    if (w < 23) { w = 23; }
    if (w > 27) { w = 27; }
    return w;
}

static int zu_fuzz_deflate_family(zu_codec c)
{
    return c == ZU_CODEC_GZIP || c == ZU_CODEC_ZLIB || c == ZU_CODEC_DEFLATE_RAW;
}

/* EXEMPTION, to remove when #64 is fixed. A DEFLATE-family stream split
   so that its last output byte arrives before its end-of-block code or
   trailer is refused at a ceiling equal to its decoded size: ZU_NEED_OUTPUT
   from codec_deflate.c's stall classification reads to the driver as the
   cap. The shape, and nothing wider: a DEFLATE codec, the input actually
   split, every decoded byte delivered intact, and then the refusal the
   ceiling's kind gives (want). regress-64-exact-cap-zlib sits inside it. */
static int zu_fuzz_issue64(const zu_fuzz_dec *o, size_t n_in,
                           const zu_fuzz_run *r, const zu_fuzz_run *whole,
                           zu_status want)
{
    return zu_fuzz_exemptions_on() &&
           zu_fuzz_deflate_family(o->codec) && o->in_chunk < n_in &&
           r->st == want && zu_fuzz_same_bytes(r, whole);
}

/* EXEMPTION, to remove when #74 is fixed. The split-decode face of
   zu_fuzz_issue74() in fuzz_oracle.h: a DEFLATE body whose code has no
   end-of-block symbol is refused at once when whole, and decoded until the
   cap when split, so the two runs disagree on the class and the bytes. The
   shape: the split run refused too, delivered bytes the whole run did not
   contradict, and zlib's reason for refusing is an incomplete code.
   split-*-raw-missing-end-of-block sits inside it. */
static int issue74_split(const zu_fuzz_dec *o, const uint8_t *in, size_t n_in,
                         const zu_fuzz_run *split, const zu_fuzz_run *whole)
{
    if (!zu_fuzz_deflate_family(o->codec) || split->st == ZU_OK ||
        whole->st == ZU_OK || !zu_fuzz_agree_so_far(split, whole)) {
        return 0;
    }
    zu_fuzz_run ref;
    size_t starts[ZU_FUZZ_MAX_MEMBERS], n;
    zu_fuzz_ref_zlib(o->codec, in, n_in, &ref, starts, &n);
    /* The split run is the one that ran on, so it is the one excused. */
    const zu_fuzz_run *longer = (split->len >= whole->len) ? split : whole;
    int hit = zu_fuzz_issue74(o->codec, in, n_in, longer, &ref);
    zu_fuzz_run_free(&ref);
    return hit;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 3) {
        return 0;
    }
    zu_fuzz_dec o;
    memset(&o, 0, sizeof(o));
    o.in_chunk  = (size_t) data[0] + 1u;
    o.out_chunk = (size_t) data[1] + 1u;
    switch (data[2] % 7) {
    case 0:  o.codec = ZU_CODEC_GZIP; break;
    case 1:  o.codec = ZU_CODEC_ZLIB; break;
    case 2:  o.codec = ZU_CODEC_DEFLATE_RAW; break;
    case 3:  o.codec = ZU_CODEC_ZSTD; break;        /* the frame prober */
    case 4:  o.codec = ZU_CODEC_LZ4_FRAME; break;
    case 5:  o.codec = ZU_CODEC_LZ4_BLOCK; break;
    default: o.codec = ZU_CODEC_IDENTITY; break;
    }
    o.finish_with_last = (data[2] / 7) & 1;
    o.flags = ZU_DEC_CONCAT_MEMBERS;
    const uint8_t *in = data + 3;
    const size_t n_in = size - 3;

    zu_fuzz_run whole, split;
    zu_fuzz_decode_whole(o.codec, o.flags, 1, in, n_in, &whole);
    zu_fuzz_decode_run(&o, in, n_in, &split);
    ZU_FUZZ_CHECK(whole.st != ZU_ERR_INTERNAL && split.st != ZU_ERR_INTERNAL,
                  "codec %d: the decoder stalled (whole %s, split %s)",
                  (int) o.codec, zu_status_string(whole.st),
                  zu_status_string(split.st));
    if (split.st == ZU_OK || whole.st == ZU_OK || split.st != whole.st) {
        if (!issue74_split(&o, in, n_in, &split, &whole)) {
            ZU_FUZZ_SAME(&split, &whole, "split decode vs whole");
        }
    } else {
        /* Both refused, for the same reason. How much they delivered first
           may differ: given a whole zstd frame and room for all of it,
           libzstd decodes it in one pass and reports nothing until the
           checksum passes, where a split decode hands bytes over as it
           goes. Neither may deliver a byte the other contradicts. */
        ZU_FUZZ_CHECK(zu_fuzz_agree_so_far(&split, &whole),
                      "codec %d: split and whole both refused (%s) but "
                      "delivered different bytes (%zu and %zu, first "
                      "difference at %zu)", (int) o.codec,
                      zu_status_string(split.st), split.len, whole.len,
                      zu_fuzz_first_diff(&split, &whole));
    }

    if (whole.st == ZU_OK && whole.len <= ZU_FUZZ_DERIVED_MAX) {
        const uint64_t n = whole.len;
        zu_fuzz_run r;

        /* max_output = N. 0 means "no limit", so N = 0 is the expected-size
           pass's to cover. */
        if (n > 0) {
            zu_fuzz_dec lim = o;
            lim.max_output = n;
            zu_fuzz_decode_run(&lim, in, n_in, &r);
            if (!(o.codec == ZU_CODEC_ZSTD && r.st == ZU_ERR_OUTPUT_LIMIT &&
                  zu_fuzz_zstd_window_too_large(in, n_in,
                                                zu_fuzz_zstd_wlog(n))) &&
                !zu_fuzz_issue64(&o, n_in, &r, &whole, ZU_ERR_OUTPUT_LIMIT)) {
                ZU_FUZZ_SAME(&r, &whole, "max_output = decoded size");
            }
            zu_fuzz_run_free(&r);

            lim.max_output = n - 1;
            if (n - 1 > 0) {
                zu_fuzz_decode_run(&lim, in, n_in, &r);
                ZU_FUZZ_CHECK(r.st == ZU_ERR_OUTPUT_LIMIT &&
                              zu_fuzz_is_prefix(&r, &whole),
                              "codec %d: max_output = %llu below the decoded "
                              "%llu gave %s with %zu bytes", (int) o.codec,
                              (unsigned long long) (n - 1),
                              (unsigned long long) n, zu_status_string(r.st),
                              r.len);
                zu_fuzz_run_free(&r);
            }
        }

        zu_fuzz_dec exp = o;
        exp.has_expected = 1;
        exp.expected = n;
        zu_fuzz_decode_run(&exp, in, n_in, &r);
        if (!zu_fuzz_issue64(&o, n_in, &r, &whole, ZU_ERR_INVALID_DATA)) {
            ZU_FUZZ_SAME(&r, &whole, "expected_size = decoded size");
        }
        zu_fuzz_run_free(&r);

        for (int d = -1; d <= 1; d += 2) {
            if (d < 0 && n == 0) { continue; }
            exp.expected = n + (uint64_t) (int64_t) d;
            zu_fuzz_decode_run(&exp, in, n_in, &r);
            ZU_FUZZ_CHECK(r.st == ZU_ERR_INVALID_DATA &&
                          zu_fuzz_is_prefix(&r, &whole),
                          "codec %d: expected_size = %llu against a decoded "
                          "%llu gave %s with %zu bytes", (int) o.codec,
                          (unsigned long long) exp.expected,
                          (unsigned long long) n, zu_status_string(r.st),
                          r.len);
            zu_fuzz_run_free(&r);
        }
    }
    zu_fuzz_run_free(&whole);
    zu_fuzz_run_free(&split);
    return 0;
}
