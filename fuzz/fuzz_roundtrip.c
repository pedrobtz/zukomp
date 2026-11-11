/* The encoders (#81): the fuzzer's bytes as plaintext, compressed by every
 * codec at a level, chunking and flush cadence the fuzzer chooses.
 *
 * Byte 0: the codec (byte 0 % 7) and which call carries ZU_FINISH (bit 3).
 * Byte 1: the level, an index into a per-codec list spanning its range.
 * Bytes 2 and 3: the input and output chunks (0 = one buffer, else
 * 1..255). Byte 4: ZU_FLUSH every (byte 4 % 8) calls, 0 for never, for the
 * codecs that advertise ZU_CAN_FLUSH. The rest is the plaintext.
 *
 * The oracles (#68):
 *
 *   1. ROUND TRIP. zukomp decodes the encoding back to the plaintext.
 *   2. EXTERNAL DECODER. The codec's reference (fuzz_oracle.h: system zlib,
 *      libzstd's one-shot decoder, liblz4) decodes it back too, so zukomp
 *      cannot round-trip through a private dialect.
 *   3. BOUND. Without ZU_FLUSH the encoding fits zu_compress_bound().
 *   4. ONE-SHOT. zu_compress_one() into a buffer of zu_compress_bound()
 *      bytes succeeds and gives the bytes the streaming encoder gives when
 *      handed everything at once, as komp_compress() does (design 18:
 *      the same input gives the same bytes). */
#include "fuzz_oracle.h"

#define ZU_FUZZ_PLAIN_MAX (256u * 1024u)

static int32_t zu_fuzz_level(zu_codec codec, uint8_t k)
{
    static const int32_t deflate[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    static const int32_t zstd[] = { -131072, -100, -5, -1, 1, 3, 6, 9, 12,
                                    ZU_LEVEL_DEFAULT };
    static const int32_t lz4[] = { -65536, -100, -1, 0, 1, 2, 3, 6, 9, 12 };
    switch (codec) {
    case ZU_CODEC_GZIP: case ZU_CODEC_ZLIB: case ZU_CODEC_DEFLATE_RAW:
        return (k % 11 == 10) ? ZU_LEVEL_DEFAULT : deflate[k % 11 % 10];
    case ZU_CODEC_ZSTD:
        return zstd[k % 10];
    case ZU_CODEC_LZ4_FRAME: case ZU_CODEC_LZ4_BLOCK:
        return lz4[k % 10];
    default:
        return ZU_LEVEL_DEFAULT;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 5) {
        return 0;
    }
    zu_fuzz_init();
    zu_codec codec;
    switch (data[0] % 7) {
    case 0:  codec = ZU_CODEC_GZIP; break;
    case 1:  codec = ZU_CODEC_ZLIB; break;
    case 2:  codec = ZU_CODEC_DEFLATE_RAW; break;
    case 3:  codec = ZU_CODEC_ZSTD; break;
    case 4:  codec = ZU_CODEC_LZ4_FRAME; break;
    case 5:  codec = ZU_CODEC_LZ4_BLOCK; break;
    default: codec = ZU_CODEC_IDENTITY; break;
    }
    const int finish_with_last = (data[0] >> 3) & 1;
    const int32_t level = zu_fuzz_level(codec, data[1]);
    const size_t in_chunk = data[2];
    const size_t out_chunk = data[3];
    unsigned flush_every = data[4] % 8;

    zu_codec_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = (uint32_t) sizeof(info);
    ZU_FUZZ_CHECK(zu_codec_get_info(codec, &info) == ZU_OK,
                  "codec %d is not registered", (int) codec);
    if (!(info.flags & ZU_CAN_FLUSH)) {
        flush_every = 0;
    }

    const uint8_t *plain = data + 5;
    size_t n = size - 5;
    if (n > ZU_FUZZ_PLAIN_MAX) { n = ZU_FUZZ_PLAIN_MAX; }

    zu_fuzz_roundtrip_check(codec, level, plain, n, in_chunk, out_chunk,
                            flush_every, finish_with_last);

    /* One-shot against streaming, the whole input in one call. */
    size_t bound = 0;
    ZU_FUZZ_CHECK(zu_compress_bound(codec, level, n, &bound) == ZU_OK,
                  "zu_compress_bound(codec %d) failed", (int) codec);
    uint8_t *one = (uint8_t *) malloc(bound ? bound : 1);
    if (one == NULL) { return 0; }
    zu_encoder_opts eo;
    memset(&eo, 0, sizeof(eo));
    eo.struct_size = (uint32_t) sizeof(eo);
    eo.codec = codec;
    eo.level = level;
    size_t written = 0;
    zu_status st = zu_compress_one(&eo, plain, n, one, bound, &written);
    ZU_FUZZ_CHECK(st == ZU_OK, "zu_compress_one(codec %d, level %d, %zu "
                  "bytes) into its bound failed: %s", (int) codec, (int) level,
                  n, zu_status_string(st));
    zu_fuzz_run whole;
    zu_fuzz_encode_run(codec, level, plain, n, 0, 0, 0, 1, &whole);
    ZU_FUZZ_CHECK(whole.len == written &&
                  (written == 0 || memcmp(whole.out, one, written) == 0),
                  "codec %d level %d: zu_compress_one gave %zu bytes, the "
                  "streaming encoder %zu different ones", (int) codec,
                  (int) level, written, whole.len);
    zu_fuzz_run_free(&whole);
    free(one);
    return 0;
}
