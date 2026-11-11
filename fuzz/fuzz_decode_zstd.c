/* Decodes arbitrary bytes as zstd, frame continuation included. libzstd is
 * fuzzed upstream; what is fuzzed here is zukomp's driving of it: the
 * stall classification, the frame prober, the window ceiling.
 *
 * The oracle is zu_fuzz_whole_stream_oracle() in fuzz_oracle.h: both
 * FINISH orders agree, libzstd's one-shot ZSTD_decompressDCtx() agrees on
 * the verdict and every byte, a refusal delivers only a valid prefix, and
 * accepted output round-trips (#68). */
#include "fuzz_oracle.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zu_fuzz_whole_stream_oracle(ZU_CODEC_ZSTD, data, size);
    return 0;
}
