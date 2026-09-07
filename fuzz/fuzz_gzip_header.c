/* The gzip header parser on its own, fed one byte at a time.
 *
 * It gets its own target because it is the highest-risk code in the
 * package: variable length, four optional fields, two of them
 * NUL-terminated and attacker-controlled. Driving it directly rather than
 * through a full decode means the fuzzer spends every input on the parser
 * instead of on DEFLATE. */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zu_int_gzip_header h;
    zu_int_gzip_header_init(&h);
    for (size_t i = 0; i < size; i++) {
        int done = 0;
        if (zu_int_gzip_header_feed(&h, data[i], &done) != ZU_OK) {
            return 0;
        }
        if (done) {
            return 0;
        }
    }
    return 0;
}
