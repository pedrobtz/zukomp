/* Detection over arbitrary bytes. It must never read past `size` -- which
   is the whole point, since sniffers index into a buffer whose length they
   were told about rather than one they measured. */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zu_fuzz_init();
    zu_codec codec = ZU_CODEC_NONE;
    (void) zu_sniff(data, size, &codec);
    return 0;
}
