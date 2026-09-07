/* Shared scaffolding for the libFuzzer targets.
 *
 * These link the pure-C core -- registry, driver, buffers, codecs, gzip
 * parser -- with no R in the process at all. That is only possible because
 * src/zu_internal.h is kept free of R; if a fuzz target ever fails to build
 * with an Rinternals.h error, something leaked into the core.
 *
 * Every target decodes into a *bounded* buffer. Fuzzing a decompressor
 * without a cap just finds the same decompression bomb over and over and
 * calls it a crash; the cap keeps findings about memory safety. */
#ifndef ZU_FUZZ_COMMON_H
#define ZU_FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zukomp.h"
#include "zu_internal.h"

#define ZU_FUZZ_MAX_OUTPUT (16u * 1024u * 1024u)
#define ZU_FUZZ_OUT_CHUNK  (64u * 1024u)

/* The registry is written once, before any stream exists -- the same
   contract R_init_zukomp honours. */
static void zu_fuzz_init(void)
{
    static int done = 0;
    if (!done) {
        zu_int_register_builtin_codecs();
        done = 1;
    }
}

/* Drives a decode to completion, or to the first error, in chunks. The
   chunk sizes are what make the boundary handling reachable: a decoder can
   be perfectly correct in one pass and wrong when a header spans two. */
static int zu_fuzz_decode(zu_codec codec, const uint8_t *data, size_t size,
                          size_t in_chunk, size_t out_chunk, uint32_t flags)
{
    zu_fuzz_init();

    if (in_chunk == 0) { in_chunk = size ? size : 1; }
    if (out_chunk == 0) { out_chunk = ZU_FUZZ_OUT_CHUNK; }

    zu_decoder_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = (uint32_t) sizeof(opts);
    opts.codec = codec;
    opts.max_output = ZU_FUZZ_MAX_OUTPUT;
    opts.flags = flags;

    zu_decoder *dec = NULL;
    if (zu_decoder_new(&dec, &opts) != ZU_OK) {
        return 0;
    }

    uint8_t *out = (uint8_t *) malloc(out_chunk);
    if (out == NULL) {
        zu_decoder_free(dec);
        return 0;
    }

    zu_buffer buf;
    memset(&buf, 0, sizeof(buf));

    size_t fed = 0;
    for (;;) {
        if (buf.src_pos == buf.src_size && fed < size) {
            size_t take = size - fed;
            if (take > in_chunk) { take = in_chunk; }
            buf.src = data + fed;
            buf.src_size = take;
            buf.src_pos = 0;
            fed += take;
        }
        int last = (fed >= size) && (buf.src_pos == buf.src_size);

        buf.dst = out;
        buf.dst_size = out_chunk;
        buf.dst_pos = 0;

        zu_status st = zu_decoder_process(dec, &buf,
                                          last ? ZU_FINISH : ZU_RUN);
        if (st != ZU_OK && st != ZU_NEED_INPUT && st != ZU_NEED_OUTPUT) {
            break;                       /* ZU_STREAM_END, or an error */
        }
        if (buf.dst_pos == 0 && last && st == ZU_NEED_INPUT) {
            break;                       /* no progress possible */
        }
    }

    free(out);
    zu_decoder_free(dec);
    return 0;
}

#endif /* ZU_FUZZ_COMMON_H */
