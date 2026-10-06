# cran-comments

## Resubmission

This is a resubmission. In response to the review of 0.1.0:

* All authors and copyright holders credited anywhere in the bundled miniz
  sources are now in `Authors@R`: Martin Raiber (`ctb`, `cph`; a separate
  copyright line on miniz's ZIP code), Alex Evans (`ctb`; the original PNG
  writer, which he released into the public domain), and Alistair Moffat and
  Jyrki Katajainen (`ctb`; the minimum-redundancy code-length routine). They
  join Rich Geldreich, Tenacious Software LLC, RAD Game Tools and Valve
  Software, who were already listed. `inst/COPYRIGHTS` now records the same
  credits alongside each copyright line.

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

* Authors and copyright holders are listed in `Authors@R` and
  `inst/COPYRIGHTS`. The
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
