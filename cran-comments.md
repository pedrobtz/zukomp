# cran-comments

## Test environments

* local: macOS Tahoe 26.6 (aarch64), R 4.6.1
* GitHub Actions: macOS, Windows and Ubuntu on R-release, Windows on
  R-devel, and Ubuntu on R-oldrel-1
* R-hub CRAN-like containers on R-devel: `clang23`, `ubuntu-clang` and
  `ubuntu-gcc16`

## R CMD check results

0 errors | 0 warnings | 1 note

* This is a new submission.

## Bundled third-party sources

The package bundles a trimmed copy of the miniz compression library (MIT)
under `src/vendor/miniz/`, so that it needs no system compression library and
no `SystemRequirements`.

* Copyright holders are listed in `Authors@R` and `inst/COPYRIGHTS`. The
  upstream licence is in `src/vendor/miniz/LICENSE`.
* Three small local patches are applied, each recorded in `inst/COPYRIGHTS`.
  They add a compile-out guard for the PNG writer, make the decoder reject
  out-of-range match distances as RFC 1951 requires, and make miniz's
  `assert()` macro overridable so it cannot abort R.

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
