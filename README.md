# zukomp

<!-- badges: start -->
[![Lifecycle: experimental](https://img.shields.io/badge/lifecycle-experimental-orange.svg)](https://lifecycle.r-lib.org/articles/stages.html#experimental)
[![R-CMD-check](https://github.com/pedrobtz/zukomp/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/pedrobtz/zukomp/actions/workflows/R-CMD-check.yaml)
[![coverage](https://raw.githubusercontent.com/pedrobtz/zukomp/gh-pages/badges/coverage.svg)](https://github.com/pedrobtz/zukomp/actions/workflows/coverage.yaml)
<!-- badges: end -->

zukomp compresses and decompresses raw vectors through a single codec-neutral
interface backed by a runtime registry, so the set of available codecs is
discovered rather than fixed at compile time. The DEFLATE family (`gzip`, `zlib`
and headerless `deflate-raw`) is built in from vendored
[miniz](https://github.com/richgel999/miniz) sources, so no system compression
library is required.

## Installation

``` r
install.packages("zukomp")
```

Or the development version from GitHub:

``` r
# install.packages("pak")
pak::pak("pedrobtz/zukomp")
```

## Usage

Bytes in, bytes out. `komp_compress()` names a codec; `komp_decompress()` detects
it by default.

``` r
library(zukomp)

x <- charToRaw(strrep("the quick brown fox ", 500))
z <- komp_compress(x, "gzip")

length(x)                        # 10000
length(z)                        # 81
identical(komp_decompress(z), x) # TRUE
```

`komp_codecs()` is the registry. Codecs this build knows the name of but does not
implement still get a row, so "install the satellite package" is a possible
answer:

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

Decompression is safe to point at untrusted input: output is capped at 1 GiB by
default, and the cap is enforced by the core stream driver, so no codec —
including a third-party one — can bypass it. The [getting started
article](https://pedrobtz.github.io/zukomp/articles/zukomp.html) covers the
limits, detection, the structured error conditions, and the C ABI that lets
another package drive these codecs or register one of its own.

## Using zukomp from C

There are two ways for another package to reach the codecs, and they suit
different consumers.

**The registered function table** is the one to prefer. Declare

```
Imports:    zukomp
LinkingTo:  zukomp
```

and include `<zukomp-r.h>`, which resolves `zukomp_api()` through
`R_GetCCallable()`. The consumer drives whatever codecs the installed zukomp
has, including ones registered after it was built, and links nothing.

**The static archive** is for a consumer that needs the ZIP *container* layer
rather than a codec: reading the members of a `.zip`, an `.xlsx` or any other
ZIP-shaped format. That layer is trimmed out of `zukomp.so` on purpose and is
not reachable through the table. An installed zukomp carries

```
zukomp/include/miniz.h
zukomp/lib/libzukomp.a          # lib/x64/ on Windows; see below
zukomp/licenses/miniz-LICENSE
```

where the archive is a second compilation of the same vendored `miniz.c`, with
the ZIP reader, `stdio` and timestamps left in. `LinkingTo: zukomp` puts the
header on the include path; the archive's location comes from
`system.file("lib", .Platform$r_arch, package = "zukomp")`, which a `configure`
script can resolve into `src/Makevars` without adding an `Imports:` dependency:

``` sh
ZUKOMP_LIB=$("${R_HOME}/bin/Rscript" -e \
  'cat(system.file("lib", .Platform$r_arch, package = "zukomp"))')
sed "s|@ZUKOMP_LIB@|${ZUKOMP_LIB}|" src/Makevars.in > src/Makevars
```

``` make
PKG_CPPFLAGS = -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
PKG_LIBS = "@ZUKOMP_LIB@/libzukomp.a"
```

`.Platform$r_arch` is `""` on every single-arch platform and `"x64"` on
Windows, where the archive installs under that sub-directory beside the shared
object's. Quote the expansion in `PKG_LIBS`: an R library path containing a
space (`C:/Program Files/R/...`) otherwise arrives at the linker as two
arguments. Windows needs the same two lines in `configure.win`, since R runs
only that file there.

Four things to know before taking this route:

- **`-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES` is required, not optional.** Without
  it `miniz.h` `#define`s `compress`, `crc32`, `adler32` and friends over
  every translation unit that includes it, colliding with the zlib the R
  process already links. The archive is built with it set, so the names are
  not there to link against either way.
- **Define nothing else.** The archive was compiled from a fixed set of
  `MINIZ_NO_*` flags, and two of them change what `miniz.h` *declares* rather
  than only what it exports. `MINIZ_NO_TIME` swaps `MZ_TIME_T` from `time_t`
  to a two-word struct, which changes the layout of
  `mz_zip_archive_file_stat` between your translation unit and the archive --
  a silent mismatch, not a link error. `MINIZ_NO_STDIO` removes the
  declaration of `mz_zip_reader_init_file()` entirely. zukomp's own
  `src/Makevars` sets both, because `zukomp.so` has no ZIP layer to describe;
  do not copy them from there.
- **The reader is all there is.** `MINIZ_NO_ARCHIVE_WRITING_APIS` stays set,
  so `mz_zip_writer_*` compiles from the header but does not link.
- **You get your own copy of miniz**, linked into your shared object, sharing
  no state with the one inside `zukomp.so`. A zukomp update does not reach it
  until you rebuild. It carries zukomp's local patches, the RFC 1951
  match-distance check among them, and was compiled by whichever toolchain
  installed zukomp -- so reinstall zukomp after changing compilers, as you
  would for any other `LinkingTo` dependency shipping object code.

miniz is MIT-licensed third-party code. An installed zukomp carries the
notice alongside it, at `zukomp/licenses/miniz-LICENSE`; `inst/COPYRIGHTS`
records the provenance.
