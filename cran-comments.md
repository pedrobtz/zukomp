# cran-comments

## Test environments

* local: macOS 15.7 (x86_64), R 4.5.2
* GitHub Actions: macOS, Windows and Ubuntu on R-release, plus Ubuntu on
  R-oldrel-1
* R-hub CRAN-like containers on R-devel: `clang23`, `ubuntu-clang` and
  `ubuntu-gcc16`, compiling with `CC += -std=gnu23` and `CFLAGS += -pedantic`

## R CMD check results

0 errors | 0 warnings | 0 notes

This is a first submission, so `--as-cran` reports the expected
"New submission" NOTE.

## Method references

There are no published references describing the methods in this package. It
implements the DEFLATE, zlib and gzip formats specified in RFC 1951, RFC 1950
and RFC 1952, which are cited under `\references` in `?komp_codecs` rather
than in `Description`: they specify the formats the package implements rather
than a method it introduces.

## Bundled third-party sources

zukomp bundles a trimmed copy of the miniz compression library (MIT licence)
under `src/vendor/miniz/`. That is deliberate: the package's purpose is to give
other packages a compression interface that needs no system compression library
and no `SystemRequirements` on any platform.

* Copyright holders are listed in `Authors@R` and in `inst/COPYRIGHTS`.
* The upstream licence is reproduced verbatim in `src/vendor/miniz/LICENSE`.
* Provenance for every bundled file — upstream repository, release tag, commit,
  archive checksum, licence and the three local patches applied — is recorded in
  `tools/vendor/manifest.tsv` and can be re-verified offline by running
  `tools/vendor/verify`.
* All three patches are recorded in `inst/COPYRIGHTS` and kept as patch files
  under `tools/patches/miniz/` rather than as edits in place, so the bundled
  tree is reproducible from the manifest. The first adds a compile-out guard
  for the PNG writer that upstream does not provide. The second makes the
  decoder reject a DEFLATE match distance reaching back further than the bytes
  emitted so far, which RFC 1951 section 3.2.5 requires but which upstream
  checks only for a non-wrapping output buffer — a configuration a streaming
  caller never uses. The third wraps miniz's `MZ_ASSERT` definition in
  `#ifndef` so it can be overridden, and the package defines it away: it
  expands to `assert()` at 26 sites reachable from malformed input, and since R
  supplies `-DNDEBUG` those are already compiled out of this and every CRAN
  build. The patch only makes that explicit, so that a `-UNDEBUG` build cannot
  `abort()` where the shipping build raises a condition. All three are written
  to be upstreamable unchanged.

The bundle is trimmed at compile time (see `src/Makevars`): for `zukomp.so`,
the ZIP reader and writer, the PNG writer, all file I/O and all clock access
are compiled out, miniz's assertions are defined away, and miniz's
zlib-compatible names are disabled so that nothing collides with the zlib that
R itself links.

## Installed files beyond the shared object

`src/install.libs.R` installs three files that are not the usual `libs/`
contents, for packages that link against zukomp through `LinkingTo`:

* `lib/libzukomp.a` (`lib/x64/` where `R_ARCH` is non-empty) — a *second*
  compilation of the same vendored `miniz.c`, with the ZIP container reader
  left in. Only a consumer needing to read a `.zip` or `.xlsx` links it; it is
  never loaded by R, and `zukomp.so` keeps its own narrower trim, which the
  package's own tests assert by auditing both symbol tables. It is built by
  `$(AR)` from an object compiled with `$(ALL_CFLAGS)`, so it is position
  independent and carries no flags this package chose.
* `include/miniz.h` — copied from `src/vendor/miniz/` rather than duplicated
  under `inst/`, so the header a consumer compiles cannot drift from the
  sources the archive was compiled from.
* `licenses/miniz-LICENSE` — miniz's MIT notice. `miniz.h` carries no
  copyright line of its own, so the notice is installed explicitly beside the
  header and the archive it covers. `inst/COPYRIGHTS` records both
  compilations and points at it.

No object code is in the source tarball: `.Rbuildignore` excludes
`src/**/*.o` and `src/*.a`, and `R CMD check` confirms it.

## Additional checking

Beyond `R CMD check`, each push runs, in CI:

* ASan + UBSan (including an exhaustive-sweep variant that halts on any UBSan
  finding), valgrind, LTO, gctorture and rchk;
* a separate consumer package that links against the C ABI, to check that a
  third-party codec registered from outside zukomp still cannot bypass the
  decompression limits;
* six libFuzzer targets over the decoders and the gzip header parser, each
  replayed against a committed regression corpus;
* interoperability against fixtures produced by external gzip/zlib
  implementations, committed rather than generated at check time.

The test suite completes in well under a minute, and the exhaustive sweeps are
gated behind an environment variable so that CRAN runs the sampled subset.
