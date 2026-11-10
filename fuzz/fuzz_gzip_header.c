/* The gzip header parser on its own, fed one byte at a time.
 *
 * It gets its own target because it is the highest-risk code in the
 * package: variable length, four optional fields, two of them
 * NUL-terminated and attacker-controlled. Driving it directly rather than
 * through a full decode means the fuzzer spends every input on the parser
 * instead of on DEFLATE.
 *
 * The oracle (#68) is zu_fuzz_ref_gzip_header() in fuzz_oracle.h, a
 * whole-buffer reading of RFC 1952 section 2.3 written apart from
 * zu_gzip.c. They must agree on the outcome (header complete, input ended
 * inside it, malformed, FHCRC mismatch) and, when complete, on the byte at
 * which the header ends: one byte early and the DEFLATE body starts inside
 * the header, one late and it loses its first byte. */
#include "fuzz_oracle.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    zu_int_gzip_header h;
    zu_int_gzip_header_init(&h);
    zu_status got = ZU_NEED_INPUT;
    size_t got_len = 0;
    for (size_t i = 0; i < size; i++) {
        int done = 0;
        zu_status st = zu_int_gzip_header_feed(&h, data[i], &done);
        if (st != ZU_OK) {
            got = st;
            break;
        }
        if (done) {
            got = ZU_OK;
            got_len = i + 1;
            break;
        }
    }

    size_t want_len = 0;
    zu_status want = zu_fuzz_ref_gzip_header(data, size, ZU_INT_GZIP_MAX_FIELD,
                                             &want_len);
    ZU_FUZZ_CHECK(got == want && got_len == want_len,
                  "gzip header: parser says %s at %zu, RFC 1952 says %s at %zu",
                  zu_status_string(got), got_len, zu_status_string(want),
                  want_len);
    return 0;
}
