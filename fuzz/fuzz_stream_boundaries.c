/* Splits its own input, using the first bytes as the chunk sizes.
 *
 * Chunk-boundary handling is the property zuhttp depends on and the one
 * that rots silently, and it is not reachable by a fuzzer that always hands
 * over the whole buffer at once. Letting the fuzzer choose the split makes
 * the state machines' resumption paths part of the search space. */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 3) {
        return 0;
    }
    /* 1..255 rather than 0, so a chunk size is always a real split. */
    size_t in_chunk  = (size_t) data[0] + 1u;
    size_t out_chunk = (size_t) data[1] + 1u;
    zu_codec codec;
    switch (data[2] % 7) {
    case 0:  codec = ZU_CODEC_GZIP; break;
    case 1:  codec = ZU_CODEC_ZLIB; break;
    case 2:  codec = ZU_CODEC_DEFLATE_RAW; break;
    case 3:  codec = ZU_CODEC_ZSTD; break;        /* the frame prober */
    case 4:  codec = ZU_CODEC_LZ4_FRAME; break;
    case 5:  codec = ZU_CODEC_LZ4_BLOCK; break;
    default: codec = ZU_CODEC_IDENTITY; break;
    }
    return zu_fuzz_decode(codec, data + 3, size - 3, in_chunk, out_chunk,
                          ZU_DEC_CONCAT_MEMBERS);
}
