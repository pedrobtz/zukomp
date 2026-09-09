# zukomp

zukomp compresses and decompresses raw vectors through a single
codec-neutral interface backed by a runtime registry, so the set of
available codecs is discovered rather than fixed at compile time. The
DEFLATE family (`gzip`, `zlib` and headerless `deflate-raw`) is built in
from vendored [miniz](https://github.com/richgel999/miniz) sources, so
no system compression library is required.

## Installation

Install the development version from GitHub:

``` r

# install.packages("pak")
pak::pak("pedrobtz/zukomp")
```

## Usage

Bytes in, bytes out.
[`komp_compress()`](https://pedrobtz.github.io/zukomp/reference/komp_compress.md)
names a codec;
[`komp_decompress()`](https://pedrobtz.github.io/zukomp/reference/komp_decompress.md)
detects it by default.

``` r

library(zukomp)

x <- charToRaw(strrep("the quick brown fox ", 500))
z <- komp_compress(x, "gzip")

length(x)                        # 10000
length(z)                        # 81
identical(komp_decompress(z), x) # TRUE
```

[`komp_codecs()`](https://pedrobtz.github.io/zukomp/reference/komp_codecs.md)
is the registry. Codecs this build knows the name of but does not
implement still get a row, so “install the satellite package” is a
possible answer:

``` r

komp_codecs()[, c("id", "available", "content_encoding", "source")]
#>             id available content_encoding source
#> 1     identity      TRUE         identity zukomp
#> 2  deflate-raw      TRUE             <NA> zukomp
#> 3         zlib      TRUE          deflate zukomp
#> 4         gzip      TRUE             gzip zukomp
#> 5       brotli     FALSE               br   <NA>
#> ...
```

Decompression is safe to point at untrusted input: output is capped at 1
GiB by default, and the cap is enforced by the core stream driver, so no
codec — including a third-party one — can bypass it. The [getting
started article](https://pedrobtz.github.io/zukomp/articles/zukomp.html)
covers the limits, detection, the structured error conditions, and the C
ABI that lets another package drive these codecs or register one of its
own.
