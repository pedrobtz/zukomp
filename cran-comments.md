# cran-comments

## Submission

This is an update, from 0.1.0 to 0.2.0. It adds the zstd, lz4-frame and
lz4-block codecs, built from bundled Zstandard 1.5.7 and LZ4 1.10.0 sources.
`Authors@R` and `inst/COPYRIGHTS` credit their authors and copyright holders.
The bundled sources are described below.

## Test environments

* local: macOS Tahoe 26.6 (aarch64), R 4.6.1
* GitHub Actions: macOS, Windows and Ubuntu on R-release, Windows on
  R-devel, and Ubuntu on R-oldrel-1
* R-hub CRAN-like containers on R-devel: `clang23`, `ubuntu-clang` and
  `ubuntu-gcc16`

## R CMD check results

0 errors | 0 warnings | 0 notes

## Bundled third-party sources

The package bundles a trimmed copy of the miniz compression library (MIT)
under `src/vendor/miniz/`, so that it needs no system compression library and
no `SystemRequirements`.

Since 0.1.0 it also bundles Zstandard 1.5.7 (BSD-3-Clause, taken from its
BSD/GPLv2 dual licence as BSD only) under `src/vendor/zstd/` and the LZ4
library 1.10.0 (BSD-2-Clause) under `src/vendor/lz4/` (#55). Their copyright
holders -- Meta Platforms, Inc. and affiliates, Yann Collet and Tino
Reichardt -- are in `Authors@R` and `inst/COPYRIGHTS`; `LICENSE.note`
summarises all three licences, and each licence is installed under
`licenses/`. Each library carries one local patch, which removes a
`#pragma ... diagnostic ignored` and changes no code. Neither library's public
functions are exported from the shared object, which still exports
`R_init_zukomp` only.

* Source tarball: 797 KB (223 KB for 0.1.0), measured 2026-10-21 with
  `R CMD build` on the development version.
* Installed size: 1.4 MB, of which 924 KB is the shared object.
* Install time from the tarball: 27 s on macOS (x86_64, clang `-O2`).

* Authors and copyright holders are listed in `Authors@R` and
  `inst/COPYRIGHTS`. The
  upstream licence is in `src/vendor/miniz/LICENSE`.
* Five small local patches are applied, each recorded in `inst/COPYRIGHTS`.
  They add a compile-out guard for the PNG writer, make the decoder reject
  out-of-range match distances and blocks with no end-of-block code as
  RFC 1951 requires, make miniz's `assert()` macro overridable so it
  cannot abort R, and make the ZIP reader in the static library refuse
  four hostile archives it accepted or hung on (#92).

## Installed files beyond the shared object

For packages that link to zukomp through `LinkingTo`, `src/install.libs.R`
also installs:

* `lib/libzukomp.a` (under `R_ARCH`): a second, position-independent build of
  the bundled miniz with its ZIP reader enabled. R never loads it.
* `include/miniz.h`, the matching header.
* `licenses/miniz-LICENSE`, miniz's MIT notice, since `miniz.h` carries none.

The source tarball contains no object code.

## Reverse dependencies

None on CRAN.
