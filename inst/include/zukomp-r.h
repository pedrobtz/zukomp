/* zukomp's R-facing linkage header.
 *
 * Include this instead of zukomp.h when you are a *package* consuming
 * zukomp's C ABI. It pulls in zukomp.h, then adds the one piece that
 * unavoidably knows about R: how to get hold of the function table. The
 * table's type, zukomp_api_v1, is in zukomp.h, so code that is only handed
 * the pointer can include that header alone (#132).
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
 *     if (api == NULL) { ... this zukomp has a different ABI version ... }
 *     zu_codec c = api->codec_lookup("gzip");
 *
 * DESCRIPTION needs both:
 *
 *     Imports:   zukomp        # loads the DLL, providing the C-callable
 *     LinkingTo: zukomp        # provides these headers
 *
 * and NAMESPACE needs a real import directive, e.g.
 * importFrom(zukomp, komp_codecs). That is what Writing R Extensions
 * documents as loading an imported package's namespace (and so its DLL)
 * before yours. On current R an Imports: entry alone has been measured to do
 * it too, but that behaviour is undocumented, so do not rely on it.
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

/* Resolves the table, lazily, and caches it.
 *
 * zukomp's DLL must already be loaded when this runs: the Imports: entry and
 * the importFrom() directive above are what arrange that. If it is not --
 * zukomp not installed, or its namespace not loaded yet -- this does not
 * return NULL. R_GetCCallable() raises an R error ("function
 * 'zukomp_get_api' not provided by package 'zukomp'"), which longjmps out of
 * this call. So call it where an R error is safe: from R_init_<pkg>, or
 * from a .Call entry point before it holds any native resource (#84).
 *
 * Call it on R's main thread only: R_GetCCallable() is R API, and the cache
 * below is a plain static. Resolve the table there -- at load, say -- and
 * hand the pointer to worker threads; every function in the table may then
 * run on any thread, as zukomp.h's thread contract says (#133).
 *
 * Returns NULL for two reasons: zukomp is loaded but its ABI version is not
 * the one this header declares, or its table is smaller than the one this
 * header describes -- an older zukomp, without entries appended since. Either
 * mismatch is then a clean error at your call site rather than a wild call
 * through a table whose layout you disagree about. A NULL is not cached, so
 * a later call asks again. */
/* `static inline`, not plain `static`: a header-defined static function is
   an unused-function warning in every translation unit that includes this
   header without calling it -- which is most of them -- and a hard failure
   for a consumer building with -Werror. Each TU still gets its own `cached`,
   exactly as before. */
static inline const zukomp_api_v1 *zukomp_api(void)
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
        /* Raises, rather than returning NULL, when zukomp's DLL has not
           registered the callable; see above. The check stays for an R
           that might one day return NULL instead. */
        resolve.fn = R_GetCCallable("zukomp", "zukomp_get_api");
        if (resolve.fn != NULL) {
            cached = resolve.get((uint32_t) ZUKOMP_ABI_VERSION);
            /* Appends do not bump the version, so a zukomp older than this
               header serves a table of the same version without the
               entries appended since: smaller than the struct this consumer
               was compiled against, and not one it can call through. */
            if (cached != NULL &&
                cached->struct_size < (uint32_t) sizeof(zukomp_api_v1)) {
                cached = NULL;
            }
        }
    }
    return cached;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZUKOMP_R_H */
