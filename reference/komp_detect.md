# Identify the codec that produced a compressed stream

Detection is a property of the codec registry, not a hardcoded list: a
codec registered by another package becomes detectable as soon as it
advertises magic bytes.

## Usage

``` r
komp_detect(x)
```

## Arguments

- x:

  A raw vector; only the leading bytes are examined.

## Value

The codec's name, or `NA_character_` if nothing matched.

## Details

Not every format can be detected. Raw DEFLATE, raw LZ4 blocks, raw
Snappy and brotli have no header to recognise, so they return `NA` and
must be named explicitly. This is deliberate: guessing wrong for a
headerless format means silently returning the wrong bytes.

`zlib` is recognised by a header *predicate* rather than a constant, so
it carries a small false-positive rate – roughly one arbitrary byte pair
in a thousand looks like a valid zlib header. Codecs with real magic are
always tested first.

## See also

[`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md),
whose `detectable` column says which codecs can be found this way.

## Examples

``` r
komp_detect(komp_compress(charToRaw("hello"), "gzip"))
#> [1] "gzip"
komp_detect(komp_compress(charToRaw("hello"), "zlib"))
#> [1] "zlib"

# headerless formats are not guessed at
komp_detect(komp_compress(charToRaw("hello"), "deflate-raw"))
#> [1] NA
```
