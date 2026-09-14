# cran-comments

## Test environments

* local: macOS 15.7 (x86_64), R 4.5.2
* GitHub Actions: ubuntu-latest, macos-latest and windows-latest, each on
  R-devel, R-release and R-oldrel-1

## R CMD check results

0 errors | 0 warnings | 0 notes

This is a first submission, so `--as-cran` reports the expected
"New submission" NOTE.

## Bundled third-party sources

zukomp bundles a trimmed copy of the miniz compression library (MIT licence)
under `src/vendor/miniz/`. That is deliberate: the package's purpose is to give
other packages a compression interface that needs no system compression library
and no `SystemRequirements` on any platform.

* Copyright holders are listed in `Authors@R` and in `inst/COPYRIGHTS`.
* The upstream licence is reproduced verbatim in `src/vendor/miniz/LICENSE`.
* Provenance for every bundled file — upstream repository, release tag, commit,
  archive checksum, licence and the two local patches applied — is recorded in
  `tools/vendor/manifest.tsv` and can be re-verified offline by running
  `tools/vendor/verify`.
* Both patches are recorded in `inst/COPYRIGHTS` and kept as patch files under
  `tools/patches/miniz/` rather than as edits in place, so the bundled tree is
  reproducible from the manifest. One adds a compile-out guard for the PNG
  writer that upstream does not provide. The other makes the decoder reject a
  DEFLATE match distance reaching back further than the bytes emitted so far,
  which RFC 1951 section 3.2.5 requires but which upstream checks only for a
  non-wrapping output buffer — a configuration a streaming caller never uses.
  Both are written to be upstreamable unchanged.

The bundle is trimmed at compile time (see `src/Makevars`): the ZIP reader and
writer, the PNG writer, all file I/O and all clock access are compiled out, and
miniz's zlib-compatible names are disabled so that nothing collides with the
zlib that R itself links.

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
