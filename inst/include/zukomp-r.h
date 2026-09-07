/* zukomp's R-facing linkage header.
 *
 * Include this instead of zukomp.h when you are a *package* consuming
 * zukomp's C ABI. It pulls in zukomp.h, then adds the one piece that
 * unavoidably knows about R: how to get hold of the function table.
 *
 * Why a table at all: LinkingTo supplies headers, not object code. Nothing
 * links across installed packages, so the ABI is delivered through R's
 * registered C-callable mechanism -- and through a single versioned table,
 * so a consumer does one lookup instead of one per function.
 *
 * Usage:
 *
 *     #include <zukomp-r.h>
 *
 *     const zukomp_api_v1 *api = zukomp_api();
 *     if (api == NULL) { ... zukomp is missing or too old ... }
 *     zu_codec c = api->codec_lookup("gzip");
 *
 * DESCRIPTION needs both:
 *
 *     Imports:   zukomp        # loads the DLL, providing the C-callable
 *     LinkingTo: zukomp        # provides these headers
 *
 * and NAMESPACE needs a real import directive, e.g.
 * importFrom(zukomp, komp_codecs). See the warning on zukomp_api() below:
 * Imports alone does not load zukomp's namespace, and without an actual
 * import the DLL may not be loaded when your R_init_ runs.
 */
#ifndef ZUKOMP_R_H
#define ZUKOMP_R_H

#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

#include "zukomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The versioned function table.
 *
 * abi_version and struct_size together are what let v2 append fields
 * safely: a v1 consumer checks the version it asked for and reads only the
 * fields it knows. Adding a *codec* appends nothing here -- codecs are
 * discovered at runtime through codec_available() and codec_list() -- which
 * is why "add brotli later" is a non-event for downstream packages. */
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;

    /* discovery */
    zu_codec    (*codec_lookup)(const char *name);
    zu_codec    (*codec_from_content_encoding)(const char *token);
    int         (*codec_available)(zu_codec codec);
    zu_status   (*codec_get_info)(zu_codec codec, zu_codec_info *out);
    zu_status   (*codec_list)(zu_codec *out, size_t cap, size_t *n_out);
    zu_status   (*sniff)(const uint8_t *buf, size_t n, zu_codec *out);

    /* streaming */
    zu_status   (*encoder_new)(zu_encoder **out, const zu_encoder_opts *opts);
    zu_status   (*encoder_process)(zu_encoder *e, zu_buffer *buf, zu_flush flush);
    zu_status   (*encoder_reset)(zu_encoder *e, const zu_encoder_opts *opts);
    void        (*encoder_free)(zu_encoder *e);

    zu_status   (*decoder_new)(zu_decoder **out, const zu_decoder_opts *opts);
    zu_status   (*decoder_process)(zu_decoder *d, zu_buffer *buf, zu_flush flush);
    zu_status   (*decoder_reset)(zu_decoder *d, const zu_decoder_opts *opts);
    void        (*decoder_free)(zu_decoder *d);

    /* one-shot */
    zu_status   (*compress_bound)(zu_codec codec, int32_t level, size_t n,
                                  size_t *out);
    zu_status   (*compress_one)(const zu_encoder_opts *opts,
                                const uint8_t *src, size_t n,
                                uint8_t *dst, size_t cap, size_t *written);
    zu_status   (*decompress_one)(const zu_decoder_opts *opts,
                                  const uint8_t *src, size_t n,
                                  uint8_t *dst, size_t cap, size_t *written);

    /* misc */
    const char *(*status_string)(zu_status status);
    uint32_t    (*abi)(void);
    zu_status   (*register_codec)(const zu_codec_vtable *vtable);
} zukomp_api_v1;

/* Resolves the table, lazily, and caches it.
 *
 * Lazily on purpose. `Imports: zukomp` in DESCRIPTION does NOT load
 * zukomp's namespace unless your NAMESPACE also contains a real
 * import()/importFrom() directive -- and without that, calling
 * R_GetCCallable("zukomp", ...) from your own R_init_ can fail because
 * zukomp's DLL is not loaded yet. Resolving on first use instead of at DLL
 * init sidesteps the ordering problem entirely.
 *
 * Returns NULL if zukomp cannot satisfy the requested ABI version, so a
 * mismatch is a clean error at your call site rather than a wild call
 * through a garbage pointer. */
static const zukomp_api_v1 *zukomp_api(void)
{
    static const zukomp_api_v1 *cached = NULL;
    if (cached == NULL) {
        /* R_GetCCallable returns DL_FUNC, i.e. void (*)(void). Casting that
           directly to the real signature is what every example does, and
           modern compilers reject it under -Wcast-function-type-mismatch,
           which -Wall -Wextra -Werror turns into a build failure for the
           *consumer*. Going through a union keeps this header usable by a
           package with strict flags -- which is the whole point of shipping
           it. */
        union {
            DL_FUNC fn;
            const zukomp_api_v1 *(*get)(uint32_t);
        } resolve;
        resolve.fn = R_GetCCallable("zukomp", "zukomp_get_api");
        if (resolve.fn != NULL) {
            cached = resolve.get((uint32_t) ZUKOMP_ABI_VERSION);
        }
    }
    return cached;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZUKOMP_R_H */
