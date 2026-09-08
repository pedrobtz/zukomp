/* A driver that replays files through LLVMFuzzerTestOneInput.
 *
 * Two reasons this exists beyond convenience. It turns the corpus into a
 * regression suite runnable anywhere clang has ASan and UBSan -- libFuzzer
 * itself is not available on every platform, notably Apple's clang. And it
 * makes a committed crasher re-checkable in one command, which is what
 * keeps fuzz findings from quietly coming back. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    int files = 0;
    for (int i = 1; i < argc; i++) {
        FILE *fh = fopen(argv[i], "rb");
        if (fh == NULL) {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            return 1;
        }
        if (fseek(fh, 0, SEEK_END) != 0) { fclose(fh); return 1; }
        long n = ftell(fh);
        if (n < 0) { fclose(fh); return 1; }
        rewind(fh);

        uint8_t *buf = (uint8_t *) malloc((size_t) n ? (size_t) n : 1);
        if (buf == NULL) { fclose(fh); return 1; }
        if (n > 0 && fread(buf, 1, (size_t) n, fh) != (size_t) n) {
            free(buf); fclose(fh); return 1;
        }
        fclose(fh);

        LLVMFuzzerTestOneInput(buf, (size_t) n);
        free(buf);
        files++;
    }
    printf("replayed %d inputs\n", files);
    return 0;
}
