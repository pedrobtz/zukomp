# The codec capability table

Every codec this build of zukomp knows the name of, whether or not an
implementation is present, plus any codec registered by another package.
This is the R-visible face of the registry: it is how you discover what
can be compressed, at what levels, and which HTTP content-coding each
codec corresponds to.

## Usage

``` r
komp_codecs()
```

## Value

A data frame with one row per codec and the columns:

- id:

  Codec name, as accepted by the `codec` argument elsewhere.

- available:

  Is an implementation registered?

- can_encode, can_decode:

  Supported directions; `NA` if unavailable.

- level_min, level_max, level_default:

  Codec-native compression levels. `NA` when the codec has no level
  axis, and when it is unavailable. Levels are not comparable between
  codecs.

- detectable:

  Can
  [`komp_detect()`](https://pedrobtz.github.io/zukomp/reference/komp_detect.md)
  (Stage 10) recognise this codec from its bytes? Headerless formats
  cannot be detected and must be named.

- content_encoding:

  The HTTP content-coding token, or `NA`.

- source:

  Package that registered the implementation, or `NA`.

## Details

A codec that is declared but not installed still gets a row, with
`available = FALSE` and `NA` for everything capability-shaped. That is
deliberate: "zstd exists but you need the zukomp.zstd package" is a more
useful answer than pretending the codec does not exist.

## Examples

``` r
codecs <- komp_codecs()
codecs[, c("id", "available", "content_encoding")]
#>              id available content_encoding
#> 1      identity      TRUE         identity
#> 2   deflate-raw      TRUE             <NA>
#> 3          zlib      TRUE          deflate
#> 4          gzip      TRUE             gzip
#> 5        brotli     FALSE               br
#> 6          zstd     FALSE             zstd
#> 7     lz4-frame     FALSE             <NA>
#> 8     lz4-block     FALSE             <NA>
#> 9  snappy-frame     FALSE             <NA>
#> 10   snappy-raw     FALSE             <NA>

# what this build can actually decompress right now
codecs$id[which(codecs$can_decode)]
#> [1] "identity"    "deflate-raw" "zlib"        "gzip"       
```
