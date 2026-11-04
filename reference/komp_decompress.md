# Decompress a raw vector

Decompress a raw vector

## Usage

``` r
komp_decompress(
  x,
  codec = "auto",
  max_output = getOption("zukomp.max_output", 1024^3),
  max_ratio = getOption("zukomp.max_ratio", NULL)
)
```

## Arguments

- x:

  A raw vector holding a complete compressed stream.

- codec:

  Codec name, as listed in
  [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md).

- max_output:

  Refuse to produce more than this many bytes. Defaults to 1 GiB,
  overridable with `options(zukomp.max_output = )`. Use `0` for
  unlimited, deliberately. Compressed input from an untrusted source can
  expand enormously, and the cap is enforced by the core stream driver,
  so no codec can bypass it.

- max_ratio:

  Refuse to expand by more than this factor. `NULL` (the default) means
  no ratio limit: legitimately compressible data routinely exceeds any
  safe-looking threshold, so this is opt-in.

## Value

A raw vector.

## See also

[`komp_compress()`](https://pedrobtz.github.io/zukomp/reference/komp_compress.md),
[`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)

## Examples

``` r
z <- komp_compress(charToRaw(strrep("data ", 200)), "gzip")
rawToChar(komp_decompress(z, "gzip"))
#> [1] "data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data data "

# a small input that would expand a long way is stopped, not allocated
bomb <- komp_compress(raw(1e6), "gzip")
length(bomb)
#> [1] 1003
try(komp_decompress(bomb, "gzip", max_output = 1024))
#> Error in komp_decompress(bomb, "gzip", max_output = 1024) : 
#>   Decompressed output exceeded `max_output`.
```
