/* Status vocabulary. Pure C: no R, no codec, no allocation. */
#include "zukomp.h"

uint32_t zu_abi_version(void)
{
    return ZUKOMP_ABI_VERSION;
}

const char *zu_status_string(zu_status status)
{
    /* No default label on purpose: -Wswitch then makes a new zu_status
       enumerator a compile-time warning here rather than a silent
       "unknown" at runtime. The trailing return covers out-of-range
       values a caller may have invented. */
    switch (status) {
    case ZU_OK:                    return "ok";
    case ZU_NEED_INPUT:            return "needs more input";
    case ZU_NEED_OUTPUT:           return "needs more output space";
    case ZU_STREAM_END:            return "end of stream";

    case ZU_ERR_INVALID_ARGUMENT:  return "invalid argument";
    case ZU_ERR_UNSUPPORTED:       return "unsupported codec or operation";
    case ZU_ERR_INVALID_DATA:      return "invalid compressed data";
    case ZU_ERR_TRUNCATED:         return "truncated input";
    case ZU_ERR_CHECKSUM:          return "checksum mismatch";
    case ZU_ERR_TRAILING:          return "unexpected trailing bytes";
    case ZU_ERR_MEMORY:            return "out of memory";
    case ZU_ERR_OUTPUT_LIMIT:      return "output size limit exceeded";
    case ZU_ERR_RATIO_LIMIT:       return "compression ratio limit exceeded";
    case ZU_ERR_INTERNAL:          return "internal error";
    }
    return "unrecognised status";
}
