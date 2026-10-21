# Compress a raw vector

Bytes in, bytes out. Character input is not accepted: encoding is a
decision the caller must make explicitly, so convert with
[`charToRaw()`](https://rdrr.io/r/base/rawConversion.html) or
[`iconv()`](https://rdrr.io/r/base/iconv.html) first.

## Usage

``` r
komp_compress(x, codec = "gzip", level = NULL)
```

## Arguments

- x:

  A raw vector.

- codec:

  Codec name, as listed in
  [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md).
  Defaults to `"gzip"`, which interoperates with everything.

- level:

  Compression level. Either a codec-native whole number, one of the
  abstract names `"fast"`, `"default"` and `"best"`, or `NULL` for the
  codec's own default.

  Numeric levels are **not** comparable between codecs: `6` means
  different things to gzip and to zstd, and
  [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
  publishes each codec's valid range. zstd takes -131072 to 22 (default
  3; negative levels trade ratio for speed). Both LZ4 codecs take
  liblz4's scale, -65536 to 12: 1 is the default fast compressor, 2 to
  12 the slower high-compression one, and a negative level `-n` is the
  fast compressor at acceleration `n + 1`. The abstract names are
  therefore the portable way to say "compress harder" – each codec
  declares where `"fast"` and `"best"` land (the `level_fast` and
  `level_best` columns), and they work on every codec, including ones
  with no level axis at all.

## Value

A raw vector.

## See also

[`komp_decompress()`](https://pedrobtz.github.io/zukomp/reference/komp_decompress.md),
[`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)

## Examples

``` r
x <- charToRaw(strrep("compress me ", 100))
z <- komp_compress(x)
length(x)
#> [1] 1200
length(z)
#> [1] 50
identical(komp_decompress(z, "gzip"), x)
#> [1] TRUE

# gzip output is deterministic: no timestamp, no filename
identical(komp_compress(x), komp_compress(x))
#> [1] TRUE

# abstract levels port across codecs; numeric ones do not
length(komp_compress(x, level = "fast"))
#> [1] 50
length(komp_compress(x, level = "best"))
#> [1] 50
length(komp_compress(x, "zstd", level = "best"))
#> [1] 33
length(komp_compress(x, "lz4-frame"))
#> [1] 53
```
