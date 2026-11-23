# the narrowing bounds are straddled, and R raises before C

    Code
      komp_decompress(z, "gzip", max_output = 2^53 + 2)
    Condition
      Error in `komp_decompress()`:
      ! `max_output` must be at most 9007199254740992, or Inf for no limit.

---

    Code
      komp_decompress(z, "gzip", max_ratio = .Machine$integer.max + 1)
    Condition
      Error in `komp_decompress()`:
      ! `max_ratio` must be at most 2147483647, or Inf for no limit.

