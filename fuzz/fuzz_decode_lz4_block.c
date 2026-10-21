/* Decodes arbitrary bytes as one bare LZ4 block, with no size given, so the
   decoder's size search runs under the 16 MiB fuzz cap. */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return zu_fuzz_decode(ZU_CODEC_LZ4_BLOCK, data, size, 0, 0,
                          ZU_DEC_REJECT_TRAILING);
}
