/* Proves the MSan job can see the bug class it exists for.
 *
 * MSan reports uninitialised data only when it reaches a branch, a syscall
 * or an uninstrumented call. A decoder that copies uninitialised bytes into
 * a buffer nobody reads is therefore silent -- which is precisely the shape
 * of the match-distance bug that
 * tools/patches/miniz/0002-validate-match-distance.patch fixes: bytes were
 * read out of miniz's never-written malloc'd dictionary, copied to the
 * output, and returned to the caller as decompressed data.
 *
 * So this canary reproduces that shape rather than doing a bare
 * uninitialised read: an unwritten heap buffer, copied to a second buffer,
 * then consumed the way fuzz_common.h's zu_fuzz_consume() consumes decoder
 * output. That validates the whole chain the job depends on -- MSan being
 * live, the interceptors seeing memcpy, and the consume step surviving the
 * optimiser -- rather than just one link of it.
 *
 * The CI job asserts this binary FAILS. If it ever passes, the fuzz job
 * that looks like it is checking for uninitialised reads is checking
 * nothing at all, which is the failure mode the sanitizer jobs in
 * native-checks.yaml already had once. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CANARY_N 4096u

static volatile uint8_t canary_sink;

int main(void)
{
    /* Never written to, the way mz_inflateInit2()'s 32 KiB dictionary was
       not: miniz_def_alloc_func() is malloc(), not calloc(). */
    uint8_t *dict = (uint8_t *) malloc(CANARY_N);
    uint8_t *out = (uint8_t *) malloc(CANARY_N);
    if (dict == NULL || out == NULL) {
        return 2;
    }

    memcpy(out, dict, CANARY_N);        /* the out-of-window match copy */

    uint8_t acc = 0;
    for (size_t i = 0; i < CANARY_N; i++) {
        acc = (uint8_t) (acc ^ out[i]);
    }
    if (acc == 0xA5u) {                 /* the branch a detector reports on */
        canary_sink++;
    }

    free(dict);
    free(out);
    printf("FAIL: canary completed with no uninitialised-read report\n");
    return 0;
}
