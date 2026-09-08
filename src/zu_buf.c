/* Checked size arithmetic.
 *
 * Every buffer computation in zukomp goes through these. The package's whole
 * threat model is that compressed input is attacker-controlled (design 20),
 * and the classic way that turns into a heap overflow is a size computation
 * that wraps: `size *= 2` on a large size, or `used + needed` overflowing
 * before it is compared against the allocation. There is no bare size
 * arithmetic anywhere else in the package, on purpose. */
#include "zu_internal.h"

zu_status zu_int_add(size_t a, size_t b, size_t *out)
{
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (b > SIZE_MAX - a) {
        return ZU_ERR_MEMORY;
    }
    *out = a + b;
    return ZU_OK;
}

zu_status zu_int_mul(size_t a, size_t b, size_t *out)
{
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    if (a != 0 && b > SIZE_MAX / a) {
        return ZU_ERR_MEMORY;
    }
    *out = a * b;
    return ZU_OK;
}

/* Next capacity for a buffer that currently holds `current` bytes and needs
   room for at least `needed` more.
 *
 * Doubles, because repeated linear growth turns a decompression into a
 * quadratic memcpy loop, but never below what is actually needed and never
 * past `cap` when one is set. Returns ZU_ERR_MEMORY rather than a wrapped
 * size when the arithmetic cannot be represented -- the caller then reports
 * a memory error instead of allocating something far too small and writing
 * past it. */
zu_status zu_int_grow(size_t current, size_t needed, size_t cap, size_t *out)
{
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }

    size_t required;
    zu_status st = zu_int_add(current, needed, &required);
    if (st != ZU_OK) {
        return st;
    }
    if (cap != 0 && required > cap) {
        return ZU_ERR_OUTPUT_LIMIT;
    }

    /* Doubling may overflow where `required` did not; that is not fatal, it
       just means doubling is off the table and we grow to exactly what is
       required. */
    size_t doubled;
    if (zu_int_mul(current, 2, &doubled) != ZU_OK) {
        doubled = required;
    }

    size_t next = (doubled > required) ? doubled : required;
    if (next < ZU_INT_MIN_BUFFER) {
        next = ZU_INT_MIN_BUFFER;
    }
    if (cap != 0 && next > cap) {
        next = cap;          /* clamp: `required <= cap` was checked above */
    }

    *out = next;
    return ZU_OK;
}
