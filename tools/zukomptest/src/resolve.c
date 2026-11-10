/* What zukomp-r.h's resolution does in its two failure cases (#84), probed
 * directly so each is tested on its own: a version zukomp does not serve,
 * and a callable zukomp never registered. */
#include <zukomp-r.h>

typedef const zukomp_api_v1 *(*zukomptest_get_api)(uint32_t);

/* zukomp_get_api(version) through the registered callable: TRUE when it
   hands back a table, FALSE for NULL. */
SEXP zukomptest_api_for_version(SEXP version)
{
    union {
        DL_FUNC fn;
        zukomptest_get_api get;
    } resolve;
    resolve.fn = R_GetCCallable("zukomp", "zukomp_get_api");
    const zukomp_api_v1 *api = resolve.get((uint32_t) Rf_asInteger(version));
    return Rf_ScalarLogical(api != NULL);
}

/* R_GetCCallable() on a name zukomp did not register: the same branch a
   consumer reaches when zukomp's DLL has not registered zukomp_get_api.
   It raises an R error; reaching the return is the failure. */
SEXP zukomptest_resolve_callable(SEXP name)
{
    DL_FUNC fn = R_GetCCallable("zukomp", CHAR(STRING_ELT(name, 0)));
    return Rf_ScalarLogical(fn == NULL);
}
