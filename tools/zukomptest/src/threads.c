/* The thread contract of zukomp_api_v1, through the real table (#133).
 *
 * zugodb fetches the table once with zukomp_api() and calls it from its SQL
 * engine's worker threads; this entry point does the same. The table is
 * resolved here, on R's main thread, before any thread starts. The workers
 * run zkt_threads.h, which calls nothing but the table -- no R API -- and
 * every R call below happens after they have all been joined. ctest compiles
 * the same body under ThreadSanitizer (ctest/test_threads.c). */
#ifdef _WIN32
/* Before R's headers, which define names windows.h also uses. */
#include <windows.h>
#endif
#define R_NO_REMAP
#include <stdlib.h>

#include <R_ext/Visibility.h>

#include <zukomp-r.h>

#define ZKT_API zukomp_api_v1
#include "zkt_threads.h"

static const zukomp_api_v1 *threads_api(void)
{
    const zukomp_api_v1 *api = zukomp_api();
    if (api == NULL) {
        Rf_error("zukomptest: zukomp's API table is unavailable");
    }
    return api;
}

/* The number of comparisons made; an R error naming the first that failed. */
SEXP zukomptest_threads(SEXP r_threads, SEXP r_rounds)
{
    const zukomp_api_v1 *api = threads_api();
    char msg[256] = "";
    long checks = 0;
    int rc = zkt_thr_run(api, Rf_asInteger(r_threads), Rf_asInteger(r_rounds),
                         &checks, msg, sizeof(msg));
    if (rc != 0) {
        Rf_error("zukomptest: %s", msg);
    }
    return Rf_ScalarReal((double) checks);
}

/* A Parquet page at sink sizes n - 1, n and n + 1; see zkt_thr_page_caps(). */
SEXP zukomptest_page_caps(void)
{
    const zukomp_api_v1 *api = threads_api();
    char msg[256] = "";
    long checks = 0;
    if (zkt_thr_page_caps(api, &checks, msg, sizeof(msg)) != 0) {
        Rf_error("zukomptest: %s", msg);
    }
    return Rf_ScalarReal((double) checks);
}
