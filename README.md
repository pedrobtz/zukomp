
# zukomp

<!-- badges: start -->
[![R-CMD-check](https://github.com/pedrobtz/zukomp/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/pedrobtz/zukomp/actions/workflows/R-CMD-check.yaml)
<!-- badges: end -->

A small, portable, extensible compression package for R. One API over many
codecs, vendored so there is no system dependency, with a stable C ABI other
packages can build on.

**zukomp is a codec registry that ships with DEFLATE, not a DEFLATE package
with room for extras.** Adding a codec — including one that lives in someone
else's package — requires no change to the public header and no ABI bump.

## Installation

``` r
# install.packages("pak")
pak::pak("pedrobtz/zukomp")
```

No system compression library is required. miniz is vendored, trimmed, and
its provenance is reproducible from `tools/vendor/manifest.tsv`.

## Usage

Bytes in, bytes out.

``` r
library(zukomp)

x <- charToRaw(strrep("the quick brown fox ", 500))
z <- komp_compress(x, "gzip")

length(x)                        # 10000
length(z)                        # 81
identical(komp_decompress(z), x) # TRUE
```

`komp_decompress()` detects the codec by default, and refuses to guess when
a format has no header to recognise:

``` r
komp_detect(z)                                  # "gzip"
komp_detect(komp_compress(x, "deflate-raw"))    # NA -- nothing to detect
```

### What is available

`komp_codecs()` is the registry, and the reason this package can honestly
call itself extensible. Codecs it knows the name of but does not implement
still get a row, so "install the satellite package" is a possible answer:

``` r
komp_codecs()[, c("id", "available", "content_encoding", "source")]
#>             id available content_encoding     source
#> 1     identity      TRUE         identity     zukomp
#> 2  deflate-raw      TRUE             <NA>     zukomp
#> 3         zlib      TRUE          deflate     zukomp
#> 4         gzip      TRUE             gzip     zukomp
#> 5       brotli     FALSE               br       <NA>
#> ...
```

### Untrusted input

Compressed input from the network can expand enormously. Decompression is
capped at 1 GiB by default, and the cap is enforced by the core stream
driver, so no codec — including a third-party one — can bypass it:

``` r
bomb <- komp_compress(raw(10e6), "gzip")
length(bomb)                                    # 9737
komp_decompress(bomb, max_output = 1024)
#> Error: Decompressed output exceeded `max_output`.
```

Failures are structured conditions, not just messages. `zukomp_truncated`,
`zukomp_checksum_error`, `zukomp_invalid_data`, `zukomp_output_limit` and
the rest all carry `codec`, `input_bytes`, `output_bytes` and
`native_status`, so callers can branch on what went wrong rather than
matching on text.

## For package authors

zukomp exposes a versioned C ABI through R's registered C-callable
mechanism. A consumer needs both `Imports: zukomp` (to load the DLL) and
`LinkingTo: zukomp` (for the headers), plus a real `importFrom()` directive:

``` c
#include <zukomp-r.h>

const zukomp_api_v1 *api = zukomp_api();   // resolved lazily, cached
zu_codec c = api->codec_lookup("gzip");
```

A package can also **register a codec of its own** at
`ZU_CODEC_VENDOR_BASE`, and it becomes a first-class citizen of
`komp_codecs()` — with zukomp's output and ratio limits applying to it
automatically. `tests/consumer/zukomptest` does exactly this, as a test.

## Scope

In: `identity`, `deflate-raw`, `zlib`, `gzip`; streaming; detection;
concatenated gzip members; decompression limits; the C ABI.

Not in: ZIP and other archive formats, tar, PNG, encryption, filesystem
archive APIs, and a zlib-compatible ABI. Brotli, zstd, LZ4 and Snappy are
planned as separate satellite packages, so their licences and source size
stay out of this one.

## License

MIT. Bundled third-party sources and their copyright holders are listed in
`inst/COPYRIGHTS`.
