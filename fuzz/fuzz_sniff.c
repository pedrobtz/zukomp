/* Detection over arbitrary bytes. It must never read past `size` -- which
 * is the whole point, since sniffers index into a buffer whose length they
 * were told about rather than one they measured.
 *
 * The oracle (#68) is the formats' documented magic numbers, written here
 * from the specifications rather than read from the registry:
 *
 *   zstd       28 B5 2F FD       RFC 8878 section 3.1.1
 *   lz4-frame  04 22 4D 18       LZ4 frame format, "Magic Number"
 *   gzip       1F 8B             RFC 1952 section 2.3.1
 *   zlib       CMF FLG with CM = 8, CINFO <= 7 and (CMF*256 + FLG) % 31
 *              == 0              RFC 1950 section 2.2 (FDICT included: the
 *                                stream is zlib, only undecodable here)
 *
 * and nothing else is ever detected: not raw DEFLATE, lz4-block or
 * identity, which have no header (design 5), and not a skippable frame
 * (50..5F 2A 4D 18), which zstd and LZ4 share and so names neither. The
 * four magics cannot overlap: 28 and 04 fail zlib's CM, and 1F fails it
 * too, so the order of the checks below does not matter. */
#include "fuzz_oracle.h"

static zu_codec zu_fuzz_spec_sniff(const uint8_t *p, size_t n)
{
    static const uint8_t zstd[4] = { 0x28, 0xB5, 0x2F, 0xFD };
    static const uint8_t lz4[4]  = { 0x04, 0x22, 0x4D, 0x18 };
    if (n >= 4 && memcmp(p, zstd, 4) == 0) { return ZU_CODEC_ZSTD; }
    if (n >= 4 && memcmp(p, lz4, 4) == 0)  { return ZU_CODEC_LZ4_FRAME; }
    if (n >= 2 && p[0] == 0x1F && p[1] == 0x8B) { return ZU_CODEC_GZIP; }
    if (n >= 2 && (p[0] & 0x0F) == 8 && (p[0] >> 4) <= 7 &&
        (((unsigned) p[0] << 8) | p[1]) % 31 == 0) {
        return ZU_CODEC_ZLIB;
    }
    return ZU_CODEC_NONE;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zu_fuzz_init();
    zu_codec got = ZU_CODEC_NONE;
    zu_status st = zu_sniff(data, size, &got);
    zu_codec want = zu_fuzz_spec_sniff(data, size);
    ZU_FUZZ_CHECK(got == want && st == (want == ZU_CODEC_NONE ? ZU_ERR_UNSUPPORTED
                                                              : ZU_OK),
                  "sniff of %zu bytes: zukomp says codec %d (%s), the "
                  "formats say %d", size, (int) got, zu_status_string(st),
                  (int) want);
    return 0;
}
