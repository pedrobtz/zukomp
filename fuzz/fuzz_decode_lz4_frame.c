/* Decodes arbitrary bytes as LZ4 frames, frame continuation included. */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return zu_fuzz_decode(ZU_CODEC_LZ4_FRAME, data, size, 0, 0,
                          ZU_DEC_CONCAT_MEMBERS | ZU_DEC_REJECT_TRAILING);
}
