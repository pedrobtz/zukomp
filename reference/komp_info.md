# Build and provenance information

What this build of zukomp actually contains: its version, the ABI
version other packages link against, the codecs it registered, and the
vendored sources compiled into it with the trim applied to them.

## Usage

``` r
komp_info()
```

## Value

A list with `version`, `abi_version`, `codecs`, `vendored` and
`build_flags`. `version` is a character string rather than a
`package_version` object: this is diagnostic output that mostly ends up
pasted into a log line or a bug report, and
[`paste()`](https://rdrr.io/r/base/paste.html) on a `package_version`
needs an [`as.character()`](https://rdrr.io/r/base/character.html) at
every call site. Compare against
[`utils::packageVersion()`](https://rdrr.io/r/utils/packageDescription.html)
with `package_version(komp_info()$version)` if you need ordering.

## Details

Reported from the compiled library rather than read from
`tools/vendor/manifest.tsv`, because the manifest is not installed and
what matters here is what was actually built.

## Examples

``` r
info <- komp_info()
info$version
#> [1] "0.1.0.9000"
info$vendored
#>   source version
#> 1  miniz  11.3.2
info$build_flags
#> [1] "MINIZ_NO_ARCHIVE_APIS"          "MINIZ_NO_ARCHIVE_WRITING_APIS" 
#> [3] "MINIZ_NO_STDIO"                 "MINIZ_NO_TIME"                 
#> [5] "MINIZ_NO_ZLIB_COMPATIBLE_NAMES" "MINIZ_NO_PNG_APIS"             
#> [7] "MINIZ_NO_ASSERT"               
```
