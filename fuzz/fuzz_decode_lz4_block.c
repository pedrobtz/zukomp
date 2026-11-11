/* Decodes arbitrary bytes as one bare LZ4 block, with no size given, so
 * the decoder's size search runs under the 16 MiB fuzz cap.
 *
 * The oracle is zu_fuzz_whole_stream_oracle() in fuzz_oracle.h: both
 * FINISH orders agree, LZ4_decompress_safe() at the block's exact size
 * agrees on the verdict and every byte, a refusal delivers only a valid
 * prefix, and accepted output round-trips (#68). */
#include "fuzz_oracle.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zu_fuzz_whole_stream_oracle(ZU_CODEC_LZ4_BLOCK, data, size);
    return 0;
}
