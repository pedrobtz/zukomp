/* Decodes arbitrary bytes as zstd, frame continuation included. libzstd is
   fuzzed upstream on its own; what is fuzzed here is zukomp's driving of
   it -- the stall classification, the frame prober, the window ceiling --
   with the core's limits in force. */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    return zu_fuzz_decode(ZU_CODEC_ZSTD, data, size, 0, 0,
                          ZU_DEC_CONCAT_MEMBERS | ZU_DEC_REJECT_TRAILING);
}
