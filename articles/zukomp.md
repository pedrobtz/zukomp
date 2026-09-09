# Getting started with zukomp

``` r

library(zukomp)
```

`zukomp` is a **codec registry that ships with DEFLATE, not a DEFLATE
package with room for extras**. Everything goes through two functions
that take raw vectors and return raw vectors; which codec does the work
is a runtime lookup, so adding one — including one that lives in someone
else’s package — requires no change to the public header and no ABI
bump.

## Compressing and decompressing

[`komp_compress()`](https://pedrobtz.github.io/zukomp/reference/komp_compress.md)
names a codec. `level` is optional and codec-specific; the default is
whatever that codec considers balanced.

``` r

x <- charToRaw(strrep("the quick brown fox ", 500))

z <- komp_compress(x, "gzip")
c(input = length(x), output = length(z))
#>  input output 
#>  10000     81

identical(komp_decompress(z), x)
#> [1] TRUE
```

Raw vectors only. Character input is refused, because the encoding to
compress is the caller’s decision and not one this package should make
quietly.

``` r

komp_compress("the quick brown fox")
#> Error in `komp_compress()`:
#> ! `x` must be a raw vector, not character. zukomp is bytes in, bytes out; convert text with charToRaw() so the encoding is your decision.
```

Concatenated gzip members are a single stream, as the format intends:

``` r

both <- c(komp_compress(charToRaw("hello "), "gzip"),
          komp_compress(charToRaw("world"), "gzip"))
rawToChar(komp_decompress(both))
#> [1] "hello world"
```

## Detection, and refusing to guess

[`komp_decompress()`](https://pedrobtz.github.io/zukomp/reference/komp_decompress.md)
defaults to `codec = "auto"`, which calls
[`komp_detect()`](https://pedrobtz.github.io/zukomp/reference/komp_detect.md).
Detection is a registry property rather than a table of magic bytes kept
in one place, and it refuses to guess: a format with no header to
recognise returns `NA` and has to be named.

``` r

komp_detect(z)
#> [1] "gzip"
komp_detect(komp_compress(x, "deflate-raw"))
#> [1] NA
```

That is deliberate. Guessing wrong on a headerless format does not raise
an error — it returns wrong bytes.

``` r

komp_decompress(komp_compress(x, "deflate-raw"))
#> Error in `komp_decompress()`:
#> ! Could not identify a codec from these bytes. Headerless formats such as "deflate-raw" cannot be detected and must be named explicitly via `codec`.
```

## The registry

[`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
is the capability table, and the reason this package can honestly call
itself extensible. Codecs it knows the name of but does not implement
still get a row, so “install the satellite package” is a possible answer
rather than an unrecognised name.

``` r

komp_codecs()
#>              id available can_encode can_decode level_min level_max
#> 1      identity      TRUE       TRUE       TRUE        NA        NA
#> 2   deflate-raw      TRUE       TRUE       TRUE         0         9
#> 3          zlib      TRUE       TRUE       TRUE         0         9
#> 4          gzip      TRUE       TRUE       TRUE         0         9
#> 5        brotli     FALSE         NA         NA        NA        NA
#> 6          zstd     FALSE         NA         NA        NA        NA
#> 7     lz4-frame     FALSE         NA         NA        NA        NA
#> 8     lz4-block     FALSE         NA         NA        NA        NA
#> 9  snappy-frame     FALSE         NA         NA        NA        NA
#> 10   snappy-raw     FALSE         NA         NA        NA        NA
#>    level_default detectable content_encoding source
#> 1             NA      FALSE         identity zukomp
#> 2              6      FALSE             <NA> zukomp
#> 3              6       TRUE          deflate zukomp
#> 4              6       TRUE             gzip zukomp
#> 5             NA         NA               br   <NA>
#> 6             NA         NA             zstd   <NA>
#> 7             NA         NA             <NA>   <NA>
#> 8             NA         NA             <NA>   <NA>
#> 9             NA         NA             <NA>   <NA>
#> 10            NA         NA             <NA>   <NA>
```

`content_encoding` is the HTTP token for the codec, which is what makes
the table useful to an HTTP client: `zlib` is `deflate` on the wire, and
a codec with no token gets `NA`.

``` r

komp_codec_available("gzip")
#> [1] TRUE
komp_codec_available("zstd")
#> [1] FALSE
```

[`komp_info()`](https://pedrobtz.github.io/zukomp/reference/komp_info.md)
reports the build itself — ABI version, which codecs are compiled in,
and the vendored sources behind them.

``` r

komp_info()
#> $version
#> [1] '0.1.0'
#> 
#> $abi_version
#> [1] 1
#> 
#> $codecs
#> [1] "identity"    "deflate-raw" "zlib"        "gzip"       
#> 
#> $vendored
#>   source version
#> 1  miniz  11.3.2
#> 
#> $build_flags
#> [1] "MINIZ_NO_ARCHIVE_APIS"          "MINIZ_NO_ARCHIVE_WRITING_APIS" 
#> [3] "MINIZ_NO_STDIO"                 "MINIZ_NO_TIME"                 
#> [5] "MINIZ_NO_ZLIB_COMPATIBLE_NAMES" "MINIZ_NO_PNG_APIS"
```

## Untrusted input

Compressed input from the network can expand enormously. Decompression
is capped at 1 GiB by default, overridable per call with `max_output` or
globally with `options(zukomp.max_output = )`. The cap is enforced by
the core stream driver, so no codec — including a third-party one — can
bypass it.

``` r

bomb <- komp_compress(raw(10e6), "gzip")
length(bomb)
#> [1] 9737

komp_decompress(bomb, max_output = 1024)
#> Error in `komp_decompress()`:
#> ! Decompressed output exceeded `max_output`.
```

`max_ratio` is the second lever, off by default, bounding output against
input rather than in absolute terms:

``` r

komp_decompress(bomb, max_ratio = 10)
#> Error in `komp_decompress()`:
#> ! Compression ratio exceeded `max_ratio`.
```

## Errors are structured conditions

Failures are conditions with classes and data, not just messages, so
callers branch on what went wrong rather than matching on text.
`zukomp_truncated`, `zukomp_checksum_error`, `zukomp_invalid_data`,
`zukomp_output_limit`, `zukomp_ratio_limit` and the rest all inherit
from `zukomp_error`.

``` r

e <- tryCatch(komp_decompress(z[1:10]), zukomp_error = function(e) e)

class(e)
#> [1] "zukomp_invalid_data" "zukomp_error"        "error"              
#> [4] "condition"
conditionMessage(e)
#> [1] "Invalid compressed data in gzip stream."
```

Each carries `codec`, `input_bytes`, `output_bytes` and `native_status`:

``` r

e[c("codec", "input_bytes", "output_bytes", "native_status")]
#> $codec
#> [1] "gzip"
#> 
#> $input_bytes
#> [1] 10
#> 
#> $output_bytes
#> [1] 0
#> 
#> $native_status
#> [1] 6
```

## For package authors

`zukomp` exposes a versioned C ABI through R’s registered C-callable
mechanism, so another package can drive the codecs from C without
vendoring miniz itself. A consumer needs both `Imports: zukomp` (to load
the DLL) and `LinkingTo: zukomp` (for the headers), plus a real
`importFrom()` directive in `NAMESPACE` — `Imports:` alone does not load
the namespace, and `R_GetCCallable()` then resolves nothing.

    # in DESCRIPTION
    Imports:    zukomp
    LinkingTo:  zukomp

``` c
#include <zukomp-r.h>

const zukomp_api_v1 *api = zukomp_api();   // resolved lazily, cached
zu_codec c = api->codec_lookup("gzip");
```

A package can also **register a codec of its own** at
`ZU_CODEC_VENDOR_BASE`, and it becomes a first-class citizen of
[`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
— with zukomp’s output and ratio limits applying to it automatically,
because they live in the stream driver rather than in each codec.
`tests/consumer/zukomptest` does exactly this, as a test.

## Scope

In: `identity`, `deflate-raw`, `zlib`, `gzip`; streaming; detection;
concatenated gzip members; decompression limits; the C ABI.

Not in: ZIP and other archive formats, tar, PNG, encryption, filesystem
archive APIs, and a zlib-compatible ABI. Brotli, zstd, LZ4 and Snappy
are planned as separate satellite packages, so their licences and source
size stay out of this one.
