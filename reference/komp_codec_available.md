# Is a codec implementation available?

Is a codec implementation available?

## Usage

``` r
komp_codec_available(codec)
```

## Arguments

- codec:

  A codec name, as listed in
  [`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)'s
  `id` column.

## Value

`TRUE` if an implementation is registered, `FALSE` if the codec is known
to zukomp but not installed. An unknown name is an error of class
`zukomp_unsupported_codec`, since it is far more likely to be a typo
than a deliberate probe.

## Examples

``` r
komp_codec_available("identity")
#> [1] TRUE

# a codec zukomp knows of, but which ships in a separate package
komp_codec_available("zstd")
#> [1] FALSE
```
