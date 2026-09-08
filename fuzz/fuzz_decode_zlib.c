/* Decodes arbitrary bytes as zlib. The decoder is the only part of this
   package that ever sees hostile input, so it is where fuzzing pays. */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return zu_fuzz_decode(ZU_CODEC_ZLIB, data, size, 0, 0,
                          ZU_DEC_CONCAT_MEMBERS | ZU_DEC_REJECT_TRAILING);
}
