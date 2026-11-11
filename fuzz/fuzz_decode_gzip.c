/* Decodes arbitrary bytes as gzip, members and trailing bytes included.
 *
 * The oracle is zu_fuzz_whole_stream_oracle() in fuzz_oracle.h: both
 * FINISH orders agree, system zlib agrees on the verdict and every byte, a
 * refusal delivers only a valid prefix, and accepted output round-trips
 * (#68). */
#include "fuzz_oracle.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zu_fuzz_whole_stream_oracle(ZU_CODEC_GZIP, data, size);
    return 0;
}
